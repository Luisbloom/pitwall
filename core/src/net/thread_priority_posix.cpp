// Adaptador POSIX de la prioridad de T1 (F1.1-11): no-op deliberado.
//
// No hay forma de subir un hilo por encima de lo normal sin privilegios:
//
//   - pthread_setschedparam con SCHED_OTHER solo admite la prioridad 0;
//   - SCHED_FIFO / SCHED_RR son tiempo real, excluido por 06-REALTIME-PIPELINE 2.3,
//     y exigen CAP_SYS_NICE o RLIMIT_RTPRIO;
//   - un nice negativo (setpriority) tambien exige CAP_SYS_NICE, y aplicarlo a
//     un solo hilo es especifico de Linux.
//
// Asi que no se intenta nada y se dice. POSIX es plataforma de compilacion en
// esta fase (RNF-091), no de ejecucion validada.
#include "pw/net/thread_priority.hpp"

namespace pw::net {

ThreadPriorityResult raiseCurrentThreadAboveNormal() noexcept
{
    return ThreadPriorityResult::Unsupported;
}

} // namespace pw::net
