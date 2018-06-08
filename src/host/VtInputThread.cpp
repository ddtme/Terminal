/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"

#include "VtInputThread.hpp"

#include "../interactivity/inc/ServiceLocator.hpp"
#include "input.h"
#include "../terminal/parser/InputStateMachineEngine.hpp"
#include "outputStream.hpp" // For ConhostInternalGetSet
#include "../terminal/adapter/InteractDispatch.hpp"
#include "../types/inc/convert.hpp"
#include "server.h"
#include "output.h"

using namespace Microsoft::Console;

// Constructor Description:
// - Creates the VT Input Thread.
// Arguments:
// - hPipe - a handle to the file representing the read end of the VT pipe.
// - inheritCursor - a bool indicating if the state machine should expect a
//      cursor positioning sequence. See MSFT:15681311.
VtInputThread::VtInputThread(_In_ wil::unique_hfile hPipe,
                             const bool inheritCursor) :
    _hFile{ std::move(hPipe) },
    _hThread{},
    _utf8Parser{ CP_UTF8 },
    _dwThreadId{ 0 },
    _exitRequested{ false },
    _exitResult{ S_OK }
{
    THROW_IF_HANDLE_INVALID(_hFile.get());

    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();

    auto pGetSet = std::make_unique<ConhostInternalGetSet>(gci);
    THROW_IF_NULL_ALLOC(pGetSet);

    auto pDispatch = std::make_unique<InteractDispatch>(std::move(pGetSet));
    THROW_IF_NULL_ALLOC(pDispatch);

    auto pEngine = std::make_shared<InputStateMachineEngine>(std::move(pDispatch), inheritCursor);
    THROW_IF_NULL_ALLOC(pEngine);

    _pInputStateMachine = std::make_unique<StateMachine>(pEngine);
    THROW_IF_NULL_ALLOC(_pInputStateMachine);
}

// Method Description:
// - Processes a buffer of input characters. The characters should be utf-8
//      encoded, and will get converted to wchar_t's to be processed by the
//      input state machine.
// Arguments:
// - charBuffer - the UTF-8 characters recieved.
// - cch - number of UTF-8 characters in charBuffer
// Return Value:
// - S_OK on success, otherwise an appropriate failure.
[[nodiscard]]
HRESULT VtInputThread::_HandleRunInput(_In_reads_(cch) const byte* const charBuffer, const int cch)
{
    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    gci.LockConsole();
    auto Unlock = wil::ScopeExit([&] { gci.UnlockConsole(); });

    try
    {
        std::unique_ptr<wchar_t[]> pwsSequence;
        unsigned int cchConsumed;
        unsigned int cchSequence;

         RETURN_IF_FAILED(_utf8Parser.Parse(charBuffer, cch, cchConsumed, pwsSequence, cchSequence));
        _pInputStateMachine->ProcessString(pwsSequence.get(), cchSequence);
    }
    CATCH_RETURN();

    return S_OK;
}

// Function Description:
// - Static function used for initializing an instance's ThreadProc.
// Arguments:
// - lpParameter - A pointer to the VtInputThread instance that should be called.
// Return Value:
// - The return value of the underlying instance's _InputThread
DWORD VtInputThread::StaticVtInputThreadProc(_In_ LPVOID lpParameter)
{
    VtInputThread* const pInstance = reinterpret_cast<VtInputThread*>(lpParameter);
    return pInstance->_InputThread();
}

// Method Description:
// - Do a single ReadFile from our pipe, and try and handle it. If handling
//      failed, throw or log, depending on what the caller wants.
// Arguments:
// - throwOnFail: If true, throw an exception if there was an error processing
//      the input recieved. Otherwise, log the error.
// Return Value:
// - <none>
void VtInputThread::DoReadInput(const bool throwOnFail)
{
    byte buffer[256];
    DWORD dwRead = 0;
    bool fSuccess = !!ReadFile(_hFile.get(), buffer, ARRAYSIZE(buffer), &dwRead, nullptr);

    // If we failed to read because the terminal broke our pipe (usually due
    //      to dying itself), close gracefully with ERROR_BROKEN_PIPE.
    // Otherwise throw an exception. ERROR_BROKEN_PIPE is the only case that
    //       we want to gracefully close in.
    if (!fSuccess)
    {

        DWORD lastError = GetLastError();
        // if (lastError == ERROR_BROKEN_PIPE)
        // {
        //     _exitResult = HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE);
        //     // This won't return. We'll be terminated.
        //     CloseConsoleProcessState();
        // }
        // else
        // {
        // }
        _exitRequested = true;
        _exitResult = HRESULT_FROM_WIN32(lastError);
        return;
    }

    HRESULT hr = _HandleRunInput(buffer, dwRead);
    if (FAILED(hr))
    {
        if (throwOnFail)
        {
            _exitResult = hr;
            _exitRequested = true;
        }
        else
        {
            LOG_IF_FAILED(hr);
        }
    }
}

// Method Description:
// - The ThreadProc for the VT Input Thread. Reads input from the pipe, and
//      passes it to _HandleRunInput to be processed by the
//      InputStateMachineEngine.
// Return Value:
// - Does not return.
DWORD VtInputThread::_InputThread()
{
    while (!_exitRequested)
    {
        DoReadInput(true);
    }
    ServiceLocator::LocateGlobals().getConsoleInformation().GetVtIo()->CloseInput();

    return _exitResult;
    // Above loop will never return.
}

// Method Description:
// - Starts the VT input thread.
[[nodiscard]]
HRESULT VtInputThread::Start()
{
    RETURN_IF_HANDLE_INVALID(_hFile.get());

    HANDLE hThread = nullptr;
    // 0 is the right value, https://blogs.msdn.microsoft.com/oldnewthing/20040223-00/?p=40503
    DWORD dwThreadId = 0;

    hThread = CreateThread(nullptr,
                           0,
                           (LPTHREAD_START_ROUTINE)VtInputThread::StaticVtInputThreadProc,
                           this,
                           0,
                           &dwThreadId);

    RETURN_IF_HANDLE_INVALID(hThread);
    _hThread.reset(hThread);
    _dwThreadId = dwThreadId;

    return S_OK;
}
