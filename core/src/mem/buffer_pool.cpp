#include "pw/mem/buffer_pool.hpp"

#include <new>
#include <stdexcept>

namespace pw::mem {
namespace {

constexpr std::size_t kSlotAlignment = 64;

/// Redondea hacia arriba al multiplo de la linea de cache: dos buffers en vuelo
/// en hilos distintos no deben compartir linea.
constexpr std::size_t roundUpToCacheLine(const std::size_t value) noexcept
{
    return ((value + kSlotAlignment - 1) / kSlotAlignment) * kSlotAlignment;
}

/// Primera generacion valida. La 0 queda reservada para «ninguna», que es la de
/// un BufferHandle por defecto.
constexpr std::uint64_t kFirstGeneration = 1;

} // namespace

void BufferPool::AlignedDelete::operator()(std::byte* p) const noexcept
{
    ::operator delete[](p, std::align_val_t{kSlotAlignment});
}

BufferPool::BufferPool(const std::size_t count, const std::size_t bufferBytes)
    : count_{count}
    , bufferBytes_{roundUpToCacheLine(bufferBytes)}
    , freeHead_{pack(BufferHandle::kNullIndex, 0)}
{
    if (count == 0) {
        throw std::invalid_argument{"BufferPool: el numero de buffers debe ser mayor que cero"};
    }
    if (bufferBytes == 0) {
        throw std::invalid_argument{"BufferPool: el tamano de buffer debe ser mayor que cero"};
    }
    if (count > BufferHandle::kNullIndex) {
        throw std::invalid_argument{"BufferPool: demasiados buffers para indexarlos en 32 bits"};
    }

    // Las dos unicas asignaciones de memoria de toda la vida del pool.
    storage_.reset(static_cast<std::byte*>(
        ::operator new[](count_ * bufferBytes_, std::align_val_t{kSlotAlignment})));
    slots_ = std::make_unique<Slot[]>(count_);

    // Todos los buffers empiezan muertos (refs = 0) y en la lista de libres. Se
    // enlazan en orden inverso para que los primeros acquire() salgan en orden
    // ascendente, que hace las trazas y los volcados mucho mas legibles.
    for (std::size_t i = count_; i-- > 0;) {
        slots_[i].state.store(makeState(kFirstGeneration, 0), std::memory_order_relaxed);
        pushFree(static_cast<std::uint32_t>(i));
    }
}

BufferPool::~BufferPool() = default;

BufferPool::Slot* BufferPool::rawSlot(const BufferHandle handle) noexcept
{
    if (!handle.valid() || handle.index() >= count_) { return nullptr; }
    return &slots_[handle.index()];
}

const BufferPool::Slot* BufferPool::rawSlot(const BufferHandle handle) const noexcept
{
    if (!handle.valid() || handle.index() >= count_) { return nullptr; }
    return &slots_[handle.index()];
}

bool BufferPool::isLive(const BufferHandle handle) const noexcept
{
    const Slot* slot = rawSlot(handle);
    if (slot == nullptr) { return false; }

    // acquire: emparejado con el release del CAS que publica el estado. Si
    // vemos el estado nuevo, vemos tambien lo que se escribio antes de el.
    const auto state = slot->state.load(std::memory_order_acquire);
    if (generationOfState(state) != handle.generation() || refsOfState(state) == 0) {
        stale_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void BufferPool::pushFree(const std::uint32_t index) noexcept
{
    // relaxed: el valor leido solo sirve como candidato del CAS, que es quien
    // impone el orden.
    std::uint64_t head = freeHead_.load(std::memory_order_relaxed);
    for (;;) {
        slots_[index].next.store(indexOf(head), std::memory_order_relaxed);
        const std::uint64_t desired = pack(index, tagOf(head) + 1);
        // release: publica tanto el enlace `next` como todo lo que este hilo
        // escribio en el buffer antes de soltarlo. Quien lo saque con acquire
        // vera las dos cosas.
        if (freeHead_.compare_exchange_weak(head, desired,
                                            std::memory_order_release,
                                            std::memory_order_relaxed)) {
            return;
        }
    }
}

std::uint32_t BufferPool::popFree() noexcept
{
    // acquire: emparejado con el release de pushFree. Garantiza que al leer
    // `next` vemos el enlace que escribio quien lo devolvio.
    std::uint64_t head = freeHead_.load(std::memory_order_acquire);
    for (;;) {
        const std::uint32_t index = indexOf(head);
        if (index == BufferHandle::kNullIndex) { return BufferHandle::kNullIndex; }

        const std::uint32_t next = slots_[index].next.load(std::memory_order_relaxed);
        // La etiqueta sube tambien al sacar: si otro hilo saco y devolvio esta
        // misma ranura entre la lectura y el CAS, la etiqueta ya no coincide y
        // el CAS falla. Es lo que elimina el ABA.
        const std::uint64_t desired = pack(next, tagOf(head) + 1);

        if (freeHead_.compare_exchange_weak(head, desired,
                                            std::memory_order_acquire,
                                            std::memory_order_acquire)) {
            return index;
        }
    }
}

BufferHandle BufferPool::acquire() noexcept
{
    const std::uint32_t index = popFree();
    if (index == BufferHandle::kNullIndex) {
        exhausted_.fetch_add(1, std::memory_order_relaxed);
        return BufferHandle{};
    }

    Slot& slot = slots_[index];

    // Quien saca un indice de la pila de libres es su UNICO dueno: nadie mas
    // puede modificar este estado ahora mismo, porque cualquier handle antiguo
    // tiene una generacion menor y sus CAS se rechazan sin escribir. Por eso
    // basta una carga relajada y un almacenamiento, sin CAS.
    //
    // La generacion NO se incrementa aqui: ya la subio el release() que mato al
    // buffer anterior. Ese es justo el punto del protocolo -la muerte invalida
    // los handles, no el siguiente nacimiento-.
    const auto state                 = slot.state.load(std::memory_order_relaxed);
    const std::uint64_t generation   = generationOfState(state);

    slot.payloadSize.store(0, std::memory_order_relaxed);
    // release: publica el estado vivo. Quien valide el handle con acquire vera
    // tambien payloadSize ya reiniciado.
    slot.state.store(makeState(generation, 1), std::memory_order_release);

    acquired_.fetch_add(1, std::memory_order_relaxed);
    inUse_.fetch_add(1, std::memory_order_relaxed);
    return BufferHandle{index, generation};
}

bool BufferPool::retain(const BufferHandle handle) noexcept
{
    Slot* slot = rawSlot(handle);
    if (slot == nullptr) {
        rejectedRetains_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::uint64_t state = slot->state.load(std::memory_order_acquire);
    for (;;) {
        // Comprobar y adquirir en el MISMO paso atomico, como weak_ptr::lock.
        // Comprobar primero y sumar despues dejaria una ventana en la que otro
        // hilo mata el buffer entre las dos operaciones, y lo resucitariamos.
        if (generationOfState(state) != handle.generation() || refsOfState(state) == 0) {
            stale_.fetch_add(1, std::memory_order_relaxed);
            rejectedRetains_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // El tope se comprueba ANTES de incrementar. refs son los 16 bits bajos
        // de la palabra: sumar 1 en el maximo acarrearia hacia la generacion y
        // cambiaria la identidad del buffer sin que nadie lo hubiera pedido.
        // Es el unico camino por el que la generacion podria moverse sin una
        // muerte de por medio, y queda cerrado aqui.
        if (refsOfState(state) >= kMaxBufferRefs) {
            rejectedRetains_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // Comprobado el tope, sumar 1 a la palabra suma 1 a refs y nada mas.
        if (slot->state.compare_exchange_weak(state, state + 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            return true;
        }
    }
}

bool BufferPool::release(const BufferHandle handle) noexcept
{
    Slot* slot = rawSlot(handle);
    if (slot == nullptr) {
        rejectedReleases_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::uint64_t state = slot->state.load(std::memory_order_acquire);
    for (;;) {
        // Un release duplicado, o sobre un handle caducado, se rechaza ANTES de
        // restar: es lo que hace imposible el underflow del contador.
        if (generationOfState(state) != handle.generation() || refsOfState(state) == 0) {
            stale_.fetch_add(1, std::memory_order_relaxed);
            rejectedReleases_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const bool last = refsOfState(state) == 1;
        // La ultima referencia mata el buffer: refs a 0 y generacion + 1, en un
        // unico CAS. Desde ese instante ningun handle antiguo es valido, y eso
        // ocurre ANTES de que el buffer entre en la lista de libres.
        const std::uint64_t desired =
            last ? makeState(nextGeneration(generationOfState(state)), 0) : state - 1;

        if (slot->state.compare_exchange_weak(state, desired,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            if (last) {
                released_.fetch_add(1, std::memory_order_relaxed);
                inUse_.fetch_sub(1, std::memory_order_relaxed);
                pushFree(handle.index());
            }
            return true;
        }
    }
}

std::span<std::byte> BufferPool::bytes(const BufferHandle handle) noexcept
{
    if (!isLive(handle)) { return {}; }
    return {storage_.get() + (static_cast<std::size_t>(handle.index()) * bufferBytes_),
            bufferBytes_};
}

std::span<const std::byte> BufferPool::bytes(const BufferHandle handle) const noexcept
{
    if (!isLive(handle)) { return {}; }
    return {storage_.get() + (static_cast<std::size_t>(handle.index()) * bufferBytes_),
            bufferBytes_};
}

void BufferPool::setPayloadSize(const BufferHandle handle, const std::size_t size) noexcept
{
    if (!isLive(handle)) { return; }
    const auto clamped = size > bufferBytes_ ? bufferBytes_ : size;
    slots_[handle.index()].payloadSize.store(static_cast<std::uint32_t>(clamped),
                                             std::memory_order_relaxed);
}

std::size_t BufferPool::payloadSize(const BufferHandle handle) const noexcept
{
    if (!isLive(handle)) { return 0; }
    return slots_[handle.index()].payloadSize.load(std::memory_order_relaxed);
}

std::span<const std::byte> BufferPool::payload(const BufferHandle handle) const noexcept
{
    const auto all = bytes(handle);
    if (all.empty()) { return {}; }
    return all.first(payloadSize(handle));
}

bool BufferPool::alive(const BufferHandle handle) const noexcept
{
    const Slot* slot = rawSlot(handle);
    if (slot == nullptr) { return false; }
    const auto state = slot->state.load(std::memory_order_acquire);
    return generationOfState(state) == handle.generation() && refsOfState(state) != 0;
}

std::uint32_t BufferPool::refCount(const BufferHandle handle) const noexcept
{
    const Slot* slot = rawSlot(handle);
    if (slot == nullptr) { return 0; }
    const auto state = slot->state.load(std::memory_order_acquire);
    if (generationOfState(state) != handle.generation()) { return 0; }
    return refsOfState(state);
}

std::uint64_t BufferPool::generationOf(const std::uint32_t index) const noexcept
{
    if (index >= count_) { return 0; }
    return generationOfState(slots_[index].state.load(std::memory_order_acquire));
}

std::size_t BufferPool::inUse() const noexcept
{
    const auto value = inUse_.load(std::memory_order_relaxed);
    return value < 0 ? 0 : static_cast<std::size_t>(value);
}

std::size_t BufferPool::available() const noexcept
{
    const auto used = inUse();
    return used >= count_ ? 0 : count_ - used;
}

BufferPoolStats BufferPool::stats() const noexcept
{
    return BufferPoolStats{acquired_.load(std::memory_order_relaxed),
                           released_.load(std::memory_order_relaxed),
                           exhausted_.load(std::memory_order_relaxed),
                           stale_.load(std::memory_order_relaxed),
                           rejectedRetains_.load(std::memory_order_relaxed),
                           rejectedReleases_.load(std::memory_order_relaxed),
                           inUse()};
}

std::size_t BufferPool::reservedBytes() const noexcept
{
    return (count_ * bufferBytes_) + (count_ * sizeof(Slot));
}

} // namespace pw::mem
