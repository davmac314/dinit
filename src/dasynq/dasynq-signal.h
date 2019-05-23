#ifndef DASYNQ_SIGNAL_INCLUDED
#define DASYNQ_SIGNAL_INCLUDED 1

#include <atomic>

#include <signal.h>
#include <setjmp.h>
#include <sys/types.h>

// Support for the standard POSIX signal mechanisms. This can be used by backends that don't
// otherwise support receiving signals. It is not particularly nice (it involves using longjmp
// out of a signal handler, which POSIX mildly frowns upon) but it's really the only viable way
// to process signals together with file descriptor / other events and obtain the full siginfo_t
// data passed to the signal handler.

namespace dasynq {

class signal_traits
{
    public:

    class sigdata_t
    {
        template <typename, bool> friend class signal_events;

        siginfo_t info;

        public:
        // mandatory:
        int get_signo() { return info.si_signo; }
        int get_sicode() { return info.si_code; }
        pid_t get_sipid() { return info.si_pid; }
        uid_t get_siuid() { return info.si_uid; }
        void * get_siaddr() { return info.si_addr; }
        int get_sistatus() { return info.si_status; }
        int get_sival_int() { return info.si_value.sival_int; }
        void * get_sival_ptr() { return info.si_value.sival_ptr; }

        // XSI
        int get_sierrno() { return info.si_errno; }

        // XSR (streams) OB (obselete)
#if !defined(__OpenBSD__)
        // Note: OpenBSD doesn't have this; most other systems do. Technically it is part of the STREAMS
        // interface.
        int get_siband() { return info.si_band; }
#endif

        void set_signo(int signo) { info.si_signo = signo; }
    };

    constexpr static bool interrupt_after_signal_add = true;
};

namespace dprivate {
namespace signal_mech {

// We need to declare and define a non-static data variable, "siginfo_p", in this header, without
// violating the "one definition rule". The only way to do that is via a template, even though we
// don't otherwise need a template here:
template <typename T = decltype(nullptr)> class sig_capture_templ
{
    public:
    static siginfo_t siginfo_cap;
    static sigjmp_buf rjmpbuf;

    static void signal_handler(int signo, siginfo_t *siginfo, void *v)
    {
        siginfo_cap = *siginfo;
        siglongjmp(rjmpbuf, 1);
    }
};
template <typename T> siginfo_t sig_capture_templ<T>::siginfo_cap;
template <typename T> sigjmp_buf sig_capture_templ<T>::rjmpbuf;

using sig_capture = sig_capture_templ<>;

inline void prepare_signal(int signo)
{
    struct sigaction the_action;
    the_action.sa_sigaction = sig_capture::signal_handler;
    the_action.sa_flags = SA_SIGINFO;
    sigfillset(&the_action.sa_mask);

    sigaction(signo, &the_action, nullptr);
}

inline sigjmp_buf &get_sigreceive_jmpbuf()
{
    return sig_capture::rjmpbuf;
}

inline void unprep_signal(int signo)
{
    signal(signo, SIG_DFL);
}

inline siginfo_t * get_siginfo()
{
    return &sig_capture::siginfo_cap;
}

} } // namespace dprivate :: signal_mech

// signal_events template.
//
// Active (watched and enabled) signals are maintained as a signal mask, which either has active
// signals in the mask or inactive signals in the mask, depending on the mask_enables parameter.
// (if mask_enables is true, active signals are in the mask). Which is more convenient depends
// exactly on how the mask will be used.
//
template <class Base, bool mask_enables = false> class signal_events : public Base
{
    sigset_t active_sigmask; // mask out unwatched signals i.e. active=0
    void * sig_userdata[NSIG];

    using sigdata_t = signal_traits::sigdata_t;

    protected:

    signal_events()
    {
        if (mask_enables) {
            sigemptyset(&active_sigmask);
        }
        else {
            sigfillset(&active_sigmask);
        }
    }

    // Get the active signal mask - identifying the set of signals which have an enabled watcher.
    // if mask_enables is true, the returned set contains the active signals; otherwise, it
    // contains all inactive signals.
    const sigset_t &get_active_sigmask()
    {
        return active_sigmask;
    }

    sigjmp_buf &get_sigreceive_jmpbuf()
    {
        return dprivate::signal_mech::get_sigreceive_jmpbuf();
    }

    // process a received signal
    void process_signal()
    {
        using namespace dprivate::signal_mech;
        std::atomic_signal_fence(std::memory_order_acquire);
        auto * sinfo = get_siginfo();
        sigdata_t sigdata;
        sigdata.info = *sinfo;

        Base::lock.lock();
        void *udata = sig_userdata[sinfo->si_signo];
        if (udata != nullptr && Base::receive_signal(*this, sigdata, udata)) {
            if (mask_enables) {
                sigdelset(&active_sigmask, sinfo->si_signo);
            }
            else {
                sigaddset(&active_sigmask, sinfo->si_signo);
            }
        }
        Base::lock.unlock();
    }

    // process a received signal, and update sigmask - which should reflect the inverse of the
    // active signal mask.
    void process_signal(sigset_t &sigmask)
    {
        using namespace dprivate::signal_mech;
        std::atomic_signal_fence(std::memory_order_acquire);
        auto * sinfo = get_siginfo();
        sigdata_t sigdata;
        sigdata.info = *sinfo;

        Base::lock.lock();
        void *udata = sig_userdata[sinfo->si_signo];
        if (udata != nullptr && Base::receive_signal(*this, sigdata, udata)) {
            if (mask_enables) {
                sigaddset(&sigmask, sinfo->si_signo);
                sigdelset(&active_sigmask, sinfo->si_signo);
            }
            else {
                sigdelset(&sigmask, sinfo->si_signo);
                sigaddset(&active_sigmask, sinfo->si_signo);
            }
        }
        Base::lock.unlock();
    }

    public:

    // Note signal should be masked before call.
    void add_signal_watch(int signo, void *userdata)
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        add_signal_watch_nolock(signo, userdata);
    }

    // Note signal should be masked before call.
    void add_signal_watch_nolock(int signo, void *userdata)
    {
        sig_userdata[signo] = userdata;
        if (mask_enables) {
            sigaddset(&active_sigmask, signo);
        }
        else {
            sigdelset(&active_sigmask, signo);
        }
        dprivate::signal_mech::prepare_signal(signo);
    }

    // Note, called with lock held:
    void rearm_signal_watch_nolock(int signo, void *userdata) noexcept
    {
        sig_userdata[signo] = userdata;
        if (mask_enables) {
            sigaddset(&active_sigmask, signo);
        }
        else {
            sigdelset(&active_sigmask, signo);
        }
    }

    void remove_signal_watch_nolock(int signo) noexcept
    {
        dprivate::signal_mech::unprep_signal(signo);
        if (mask_enables) {
            sigdelset(&active_sigmask, signo);
        }
        else {
            sigaddset(&active_sigmask, signo);
        }
        sig_userdata[signo] = nullptr;
        // No need to signal other threads
    }

    void remove_signal_watch(int signo) noexcept
    {
        std::lock_guard<decltype(Base::lock)> guard(Base::lock);
        remove_signal_watch_nolock(signo);
    }

};

}

#endif
