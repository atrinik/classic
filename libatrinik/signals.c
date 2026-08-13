/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * Fork from Crossfire (Multiplayer game for X-windows).                 *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *                                                                       *
 * This program is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 * GNU General Public License for more details.                          *
 *                                                                       *
 * You should have received a copy of the GNU General Public License     *
 * along with this program; if not, write to the Free Software           *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.             *
 *                                                                       *
 * The author can be reached at admin@atrinik.org                        *
 ************************************************************************/

/**
 * @file
 * Signals API.
 *
 * This API, when imported, will register the signals defined in
 * ::register_signals (SIGSEGV, SIGINT, etc) for interception. When any
 * one of those signals has been intercepted, the appropriate action will
 * be done, based on the signal's type - aborting for SIGSEGV, exiting
 * with an error code for others.
 *
 * @author Zoey Rose
 */

#ifdef WIN32
#define WINVER 0x502
#endif

#include "signals.h"
#include "memory.h"
#include <signal.h>

#ifdef HAVE_SIGACTION
#include <execinfo.h>
#endif

#ifdef WIN32
#include <imagehlp.h>
#endif

/**
 * The signals to register.
 */
static const int register_signals[] = {
#ifndef WIN32
    SIGHUP,
#endif
    SIGSEGV,
    SIGFPE,
    SIGILL,
    SIGTERM,
    SIGABRT};

/**
 * Prefix to use for generatedtraceback files.
 */
static char traceback_prefix[64] = {"atrinik"};

/** Whether SIGTERM should request an orderly shutdown. */
static volatile sig_atomic_t graceful_termination_enabled;

/** Whether an orderly shutdown was requested by SIGTERM. */
static volatile sig_atomic_t termination_requested;

#ifdef HAVE_SIGACTION
/** Alternate stack used by POSIX signal handlers. */
static void *alternate_signal_stack;
#endif

#ifdef WIN32
/** Registered Windows vectored exception handler. */
static PVOID vectored_exception_handler;

/** First fatal exception code, or zero before traceback handling starts. */
static volatile LONG first_exception_code;
#endif

TOOLKIT_API();

/**
 * The signal interception handler.
 * @param signum
 * ID of the signal being intercepted.
 */
static void simple_signal_handler(int signum) {
    if (signum == SIGTERM && graceful_termination_enabled != 0) {
        termination_requested = 1;
        return;
    }

    if (signum == SIGABRT) {
#ifdef WIN32
        RaiseException(STATUS_ACCESS_VIOLATION, 0, 0, 0);
#else
        return;
#endif
    }

    /* SIGSEGV, so abort instead of exiting normally. */
    if (signum == SIGSEGV) {
        abort();
    }

    exit(1);
}

#if defined(WIN32) || defined(HAVE_SIGACTION)
#ifdef WIN32
/**
 * Print the module containing an instruction address.
 * @param fp
 * Traceback output stream.
 * @param label
 * Prefix for the module fields.
 * @param address
 * Instruction address to inspect.
 */
static void write_module_details(FILE *fp, const char *label, DWORD64 address) {
    MEMORY_BASIC_INFORMATION memory;
    HMODULE module = NULL;
    char name[MAX_PATH];

    if (VirtualQuery((const void *)(uintptr_t)address, &memory, sizeof(memory)) == sizeof(memory)) {
        module = (HMODULE)memory.AllocationBase;
    }

    fprintf(fp, "%s module base: %p\n", label, (void *)module);
    if (module != NULL && GetModuleFileNameA(module, VS(name)) != 0) {
        fprintf(fp, "%s module name: %s\n", label, name);
    } else {
        fprintf(fp, "%s module name: <unknown>\n", label);
    }
    fflush(fp);
}

/**
 * Terminate after a Windows exception without running signal or CRT handlers.
 * @param code
 * Original exception code to preserve as the process exit status.
 */
static void terminate_after_exception(DWORD code) {
    TerminateProcess(GetCurrentProcess(), code);
}

/**
 * Check whether an exception represents a fatal process fault.
 * @param code
 * Windows exception code.
 * @return
 * True when Atrinik should report and terminate for the exception.
 */
static bool exception_is_fatal(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_FLT_DENORMAL_OPERAND:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_INEXACT_RESULT:
        case EXCEPTION_FLT_INVALID_OPERATION:
        case EXCEPTION_FLT_OVERFLOW:
        case EXCEPTION_FLT_STACK_CHECK:
        case EXCEPTION_FLT_UNDERFLOW:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_INVALID_DISPOSITION:
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
            return true;
        default:
            return false;
    }
}

static LONG WINAPI signal_handler(EXCEPTION_POINTERS *ExceptionInfo)
#else
static void signal_handler(int sig, siginfo_t *siginfo, void *context)
#endif
{
#ifdef WIN32
    if (!exception_is_fatal(ExceptionInfo->ExceptionRecord->ExceptionCode)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    LONG previous_exception_code =
        InterlockedCompareExchange(&first_exception_code,
                                   (LONG)ExceptionInfo->ExceptionRecord->ExceptionCode,
                                   0);
    if (previous_exception_code != 0) {
        terminate_after_exception((DWORD)previous_exception_code);
        return EXCEPTION_CONTINUE_SEARCH;
    }
#endif

    struct tm *tm;
    static time_t t = 0;
    char path[HUGE_BUF], date[MAX_BUF], *homedir;
    FILE *fp;

#ifndef WIN32
    if (sig == SIGTERM && graceful_termination_enabled != 0) {
        termination_requested = 1;
        return;
    }
#endif

#ifndef WIN32
    homedir = getenv("HOME");
#else
    homedir = getenv("APPDATA");
#endif

    if (t == 0) {
        t = time(NULL);
    }

    tm = localtime(&t);

    strftime(VS(date), "%Y_%m_%d_%H-%M-%S", tm);
    fp = NULL;
    if (homedir != NULL && homedir[0] != '\0') {
        snprintf(VS(path), "%s/.atrinik/%s-traceback-%s.txt", homedir, traceback_prefix, date);
        fp = fopen(path, "a");
    }

    if (fp == NULL) {
        snprintf(VS(path), "%s-traceback-%s.txt", traceback_prefix, date);
        fp = fopen(path, "a");

        if (fp == NULL) {
            fp = stderr;
        }
    }

#ifdef WIN32
    if (fp != stderr) {
        /* Preserve details even if a later DbgHelp operation faults. */
        if (setvbuf(fp, NULL, _IONBF, 0) != 0) {
            fflush(fp);
        }
    }
#endif

#ifdef WIN32
    switch (ExceptionInfo->ExceptionRecord->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:
            fputs("Error: EXCEPTION_ACCESS_VIOLATION\n", fp);
            break;
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            fputs("Error: EXCEPTION_ARRAY_BOUNDS_EXCEEDED\n", fp);
            break;
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            fputs("Error: EXCEPTION_DATATYPE_MISALIGNMENT\n", fp);
            break;
        case EXCEPTION_FLT_DENORMAL_OPERAND:
            fputs("Error: EXCEPTION_FLT_DENORMAL_OPERAND\n", fp);
            break;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            fputs("Error: EXCEPTION_FLT_DIVIDE_BY_ZERO\n", fp);
            break;
        case EXCEPTION_FLT_INEXACT_RESULT:
            fputs("Error: EXCEPTION_FLT_INEXACT_RESULT\n", fp);
            break;
        case EXCEPTION_FLT_INVALID_OPERATION:
            fputs("Error: EXCEPTION_FLT_INVALID_OPERATION\n", fp);
            break;
        case EXCEPTION_FLT_OVERFLOW:
            fputs("Error: EXCEPTION_FLT_OVERFLOW\n", fp);
            break;
        case EXCEPTION_FLT_STACK_CHECK:
            fputs("Error: EXCEPTION_FLT_STACK_CHECK\n", fp);
            break;
        case EXCEPTION_FLT_UNDERFLOW:
            fputs("Error: EXCEPTION_FLT_UNDERFLOW\n", fp);
            break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            fputs("Error: EXCEPTION_ILLEGAL_INSTRUCTION\n", fp);
            break;
        case EXCEPTION_IN_PAGE_ERROR:
            fputs("Error: EXCEPTION_IN_PAGE_ERROR\n", fp);
            break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            fputs("Error: EXCEPTION_INT_DIVIDE_BY_ZERO\n", fp);
            break;
        case EXCEPTION_INT_OVERFLOW:
            fputs("Error: EXCEPTION_INT_OVERFLOW\n", fp);
            break;
        case EXCEPTION_INVALID_DISPOSITION:
            fputs("Error: EXCEPTION_INVALID_DISPOSITION\n", fp);
            break;
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
            fputs("Error: EXCEPTION_NONCONTINUABLE_EXCEPTION\n", fp);
            break;
        case EXCEPTION_PRIV_INSTRUCTION:
            fputs("Error: EXCEPTION_PRIV_INSTRUCTION\n", fp);
            break;
        case EXCEPTION_STACK_OVERFLOW:
            fputs("Error: EXCEPTION_STACK_OVERFLOW\n", fp);
            break;
        default:
            fputs("Error: Unrecognized Exception\n", fp);
            break;
    }
#else
    switch (sig) {
        case SIGSEGV:
            fputs("Caught SIGSEGV: Segmentation Fault\n", fp);
            break;
        case SIGFPE:
            switch (siginfo->si_code) {
                case FPE_INTDIV:
                    fputs("Caught SIGFPE: (integer divide by zero)\n", fp);
                    break;
                case FPE_INTOVF:
                    fputs("Caught SIGFPE: (integer overflow)\n", fp);
                    break;
                case FPE_FLTDIV:
                    fputs("Caught SIGFPE: (floating-point divide by zero)\n", fp);
                    break;
                case FPE_FLTOVF:
                    fputs("Caught SIGFPE: (floating-point overflow)\n", fp);
                    break;
                case FPE_FLTUND:
                    fputs("Caught SIGFPE: (floating-point underflow)\n", fp);
                    break;
                case FPE_FLTRES:
                    fputs("Caught SIGFPE: (floating-point inexact result)\n", fp);
                    break;
                case FPE_FLTINV:
                    fputs("Caught SIGFPE: (floating-point invalid operation)\n", fp);
                    break;
                case FPE_FLTSUB:
                    fputs("Caught SIGFPE: (subscript out of range)\n", fp);
                    break;
                default:
                    fputs("Caught SIGFPE: Arithmetic Exception\n", fp);
                    break;
            }
            break;
        case SIGILL:
            switch (siginfo->si_code) {
                case ILL_ILLOPC:
                    fputs("Caught SIGILL: (illegal opcode)\n", fp);
                    break;
                case ILL_ILLOPN:
                    fputs("Caught SIGILL: (illegal operand)\n", fp);
                    break;
                case ILL_ILLADR:
                    fputs("Caught SIGILL: (illegal addressing mode)\n", fp);
                    break;
                case ILL_ILLTRP:
                    fputs("Caught SIGILL: (illegal trap)\n", fp);
                    break;
                case ILL_PRVOPC:
                    fputs("Caught SIGILL: (privileged opcode)\n", fp);
                    break;
                case ILL_PRVREG:
                    fputs("Caught SIGILL: (privileged register)\n", fp);
                    break;
                case ILL_COPROC:
                    fputs("Caught SIGILL: (coprocessor error)\n", fp);
                    break;
                case ILL_BADSTK:
                    fputs("Caught SIGILL: (internal stack error)\n", fp);
                    break;
                default:
                    fputs("Caught SIGILL: Illegal Instruction\n", fp);
                    break;
            }
            break;
        case SIGTERM:
            fputs("Caught SIGTERM: a termination request was sent to the program\n", fp);
            break;
        case SIGABRT:
            fputs("Caught SIGABRT: usually caused by an abort() or assert()\n", fp);
            break;
        default:
            break;
    }
#endif

#ifdef WIN32
    fprintf(fp,
            "Exception code: 0x%08lx\n"
            "Exception address: %p\n",
            (unsigned long)ExceptionInfo->ExceptionRecord->ExceptionCode,
            ExceptionInfo->ExceptionRecord->ExceptionAddress);
    if ((ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        ExceptionInfo->ExceptionRecord->NumberParameters >= 2) {
        const char *access_type;

        switch (ExceptionInfo->ExceptionRecord->ExceptionInformation[0]) {
            case 0:
                access_type = "read";
                break;
            case 1:
                access_type = "write";
                break;
            case 8:
                access_type = "execute";
                break;
            default:
                access_type = "unknown";
                break;
        }

        fprintf(fp,
                "Access type: %s (%llu)\n"
                "Fault address: %p\n",
                access_type,
                (unsigned long long)ExceptionInfo->ExceptionRecord->ExceptionInformation[0],
                (void *)(uintptr_t)ExceptionInfo->ExceptionRecord->ExceptionInformation[1]);
        if (ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_IN_PAGE_ERROR &&
            ExceptionInfo->ExceptionRecord->NumberParameters >= 3) {
            fprintf(fp,
                    "In-page status: 0x%08llx\n",
                    (unsigned long long)ExceptionInfo->ExceptionRecord->ExceptionInformation[2]);
        }
    }

    fflush(fp);
    write_module_details(fp,
                         "Exception",
                         (DWORD64)(uintptr_t)ExceptionInfo->ExceptionRecord->ExceptionAddress);
    fflush(fp);
    fputs("Stack trace:\n", fp);

    DWORD machine_type;
    DWORD64 program_counter;
#ifdef _WIN64
    machine_type = IMAGE_FILE_MACHINE_AMD64;
    program_counter = ExceptionInfo->ContextRecord->Rip;
#else
    machine_type = IMAGE_FILE_MACHINE_I386;
    program_counter = ExceptionInfo->ContextRecord->Eip;
#endif

    if (EXCEPTION_STACK_OVERFLOW != ExceptionInfo->ExceptionRecord->ExceptionCode) {
        STACKFRAME64 frame;
        int i;

        BOOL symbols_initialized = SymInitialize(GetCurrentProcess(), 0, 1);

        memset(&frame, 0, sizeof(frame));
        frame.AddrPC.Offset = program_counter;
        frame.AddrPC.Mode = AddrModeFlat;
#ifdef _WIN64
        frame.AddrStack.Offset = ExceptionInfo->ContextRecord->Rsp;
        frame.AddrFrame.Offset = ExceptionInfo->ContextRecord->Rbp;
#else
        frame.AddrStack.Offset = ExceptionInfo->ContextRecord->Esp;
        frame.AddrFrame.Offset = ExceptionInfo->ContextRecord->Ebp;
#endif
        frame.AddrStack.Mode = AddrModeFlat;
        frame.AddrFrame.Mode = AddrModeFlat;

        i = 0;

        while (i < 64 && StackWalk64(machine_type,
                                     GetCurrentProcess(),
                                     GetCurrentThread(),
                                     &frame,
                                     ExceptionInfo->ContextRecord,
                                     0,
                                     SymFunctionTableAccess64,
                                     SymGetModuleBase64,
                                     0)) {
            fprintf(fp, "%d: %p\n", i, (void *)(uintptr_t)frame.AddrPC.Offset);
            write_module_details(fp, "  Frame", frame.AddrPC.Offset);
            i++;
        }

        if (symbols_initialized) {
            SymCleanup(GetCurrentProcess());
        }
    } else {
        fprintf(fp, "%p\n", (void *)(uintptr_t)program_counter);
    }
#else
    {
        void *stack_traces[64];
        int i, trace_size = 0;
        char **messages = NULL;

        trace_size = backtrace(stack_traces, 64);
        messages = backtrace_symbols(stack_traces, trace_size);

        for (i = 0; i < trace_size; i++) {
            fprintf(fp, "%d: %s\n", i, messages[i]);
        }

        free(messages);
    }
#endif

    if (fp == stderr) {
        fflush(stderr);
    } else {
        fclose(fp);
    }

#ifdef WIN32
    terminate_after_exception(ExceptionInfo->ExceptionRecord->ExceptionCode);
    return EXCEPTION_CONTINUE_SEARCH;
#else
    simple_signal_handler(sig);
#endif
}

#endif

TOOLKIT_INIT_FUNC(signals) {
    size_t i;

    graceful_termination_enabled = 0;
    termination_requested = 0;
#ifdef HAVE_SIGACTION
    stack_t ss;

    HARD_ASSERT(alternate_signal_stack == NULL);
    alternate_signal_stack = xmalloc(SIGSTKSZ * 4);

    ss.ss_sp = alternate_signal_stack;
    ss.ss_size = SIGSTKSZ * 4;
    ss.ss_flags = 0;

    if (sigaltstack(&ss, NULL) != 0) {
        free(alternate_signal_stack);
        alternate_signal_stack = NULL;
        LOG(ERROR, "Could not set up alternate stack.");
        exit(1);
    }
#endif

    /* Register the signals. */
    for (i = 0; i < arraysize(register_signals); i++) {
#ifdef HAVE_SIGACTION
        struct sigaction sig_action;

        sig_action.sa_sigaction = signal_handler;
        sigemptyset(&sig_action.sa_mask);
        sig_action.sa_flags = SA_SIGINFO | SA_ONSTACK;

        if (sigaction(register_signals[i], &sig_action, NULL) != 0) {
            LOG(ERROR, "Could not register signal: %d", register_signals[i]);
            exit(1);
        }
#else
        signal(register_signals[i], simple_signal_handler);
#endif
    }

#ifndef WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

#ifdef WIN32
    first_exception_code = 0;
    HARD_ASSERT(vectored_exception_handler == NULL);
    vectored_exception_handler = AddVectoredExceptionHandler(1, signal_handler);
    if (vectored_exception_handler == NULL) {
        LOG(ERROR, "Could not register vectored exception handler.");
        exit(1);
    }
#endif
}
TOOLKIT_INIT_FUNC_FINISH

TOOLKIT_DEINIT_FUNC(signals) {
#ifdef HAVE_SIGACTION
    stack_t ss = {
        .ss_flags = SS_DISABLE,
    };

    if (sigaltstack(&ss, NULL) != 0) {
        LOG(ERROR, "Could not disable alternate signal stack.");
    } else {
        free(alternate_signal_stack);
        alternate_signal_stack = NULL;
    }
#endif

#ifdef WIN32
    if (vectored_exception_handler != NULL) {
        if (RemoveVectoredExceptionHandler(vectored_exception_handler) == 0) {
            LOG(ERROR, "Could not remove vectored exception handler.");
        } else {
            vectored_exception_handler = NULL;
        }
    }
#endif
}
TOOLKIT_DEINIT_FUNC_FINISH

void signals_set_traceback_prefix(const char *prefix) {
    snprintf(VS(traceback_prefix), "%s", prefix);
}

void signals_enable_graceful_termination(void) {
    graceful_termination_enabled = 1;
}

bool signals_termination_requested(void) {
    return termination_requested != 0;
}
