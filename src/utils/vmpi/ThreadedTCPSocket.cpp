//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Threaded TCP socket implementation optimized for speed, safety,
//          and minimal memory leaks or network vulnerabilities.
//          
//=============================================================================

#include <winsock2.h>
#include <mswsock.h>
#include "IThreadedTCPSocket.h"
#include "utllinkedlist.h"
#include "threadhelpers.h"
#include "iphelpers.h"
#include "tier1/strtools.h"

#define SEND_KEEPALIVE_INTERVAL    3000
#define KEEPALIVE_TIMEOUT          25000
#define KEEPALIVE_SENTINEL         -12345

static const int g_KeepaliveSentinel = KEEPALIVE_SENTINEL;
bool g_bHandleTimeouts = true;
bool g_bSetTCPSocketThreadPriorities = true;

#ifndef _MT
#pragma message("**** WARNING **** ThreadedTCPSocket requires multithreaded runtime libraries to be used.\n")
class MTChecker {
public:
	MTChecker() { Assert(false); }
} g_MTChecker;
#endif

//-------------------------------------------------------------------------------------------------
// TCPBind: Creates and binds a TCP socket to the given address.
//-------------------------------------------------------------------------------------------------
static SOCKET TCPBind(const CIPAddr* pAddr)
{
	SOCKET sock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (sock == INVALID_SOCKET)
	{
		Assert(false);
		return INVALID_SOCKET;
	}

	sockaddr_in addr;
	IPAddrToSockAddr(pAddr, &addr);

	if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == 0)
		return sock;

	closesocket(sock);
	return INVALID_SOCKET;
}

//-------------------------------------------------------------------------------------------------
// CTCPPacket implementation
//-------------------------------------------------------------------------------------------------
int CTCPPacket::GetUserData() const { return m_UserData; }
void CTCPPacket::SetUserData(int userData) { m_UserData = userData; }
void CTCPPacket::Release() { free(this); }

//-------------------------------------------------------------------------------------------------
// CThreadedTCPSocket implementation
//-------------------------------------------------------------------------------------------------
class CThreadedTCPSocket : public IThreadedTCPSocket
{
public:
	static IThreadedTCPSocket* Create(SOCKET iSocket, CIPAddr remoteAddr, ITCPSocketHandler* pHandler, bool bDeleteHandler)
	{
		CThreadedTCPSocket* pRet = new CThreadedTCPSocket;
		if (pRet->Init(iSocket, remoteAddr, pHandler, bDeleteHandler))
			return pRet;
		pRet->Release();
		return nullptr;
	}

	// IThreadedTCPSocket methods.
	virtual void Release() { delete this; }
	virtual CIPAddr GetRemoteAddr() const { return m_RemoteAddr; }
	virtual bool IsValid() { return !CheckErrorSignal(); }
	virtual bool Send(const void* pData, int len)
	{
		const void* pChunks[1] = { pData };
		return SendChunks(pChunks, &len, 1);
	}
	virtual bool SendChunks(void const* const* pChunks, const int* pChunkLengths, int nChunks)
	{
		if (CheckErrorSignal())
			return false;
		return InternalSend(pChunks, pChunkLengths, nChunks, true);
	}
	virtual ITCPSocketHandler* GetHandler() { return m_pHandler; }

private:
	CThreadedTCPSocket()
		: m_Socket(INVALID_SOCKET), m_pHandler(nullptr), m_bWaitingForSendCompletion(false),
		m_nBytesToReceive(-1), m_bWaitingForSize(false), m_bErrorSignal(false), m_pRecvBuffer(nullptr)
	{
		memset(&m_SendOverlapped, 0, sizeof(m_SendOverlapped));
		memset(&m_RecvOverlapped, 0, sizeof(m_RecvOverlapped));
	}
	virtual ~CThreadedTCPSocket() { Term(); }

	bool Init(SOCKET iSocket, CIPAddr remoteAddr, ITCPSocketHandler* pHandler, bool bDeleteHandler)
	{
		m_Socket = iSocket;
		m_RemoteAddr = remoteAddr;
		m_pHandler = pHandler;
		m_bDeleteHandler = bDeleteHandler;

		SetInitialSocketOptions();

		m_hExitThreadsEvent.Init(true, false);
		m_hSendCompletionEvent.Init(false, false);
		m_hReadyToSendEvent.Init(false, false);
		m_hRecvEvent.Init(false, false);

		m_SendOverlapped.hEvent = m_hSendCompletionEvent.GetEventHandle();
		m_RecvOverlapped.hEvent = m_hRecvEvent.GetEventHandle();

		DWORD dwSendThreadID, dwRecvThreadID;
		m_hSendThread = CreateThread(NULL, 0, &CThreadedTCPSocket::StaticSendThreadFn, this, CREATE_SUSPENDED, &dwSendThreadID);
		m_hRecvThread = CreateThread(NULL, 0, &CThreadedTCPSocket::StaticRecvThreadFn, this, CREATE_SUSPENDED, &dwRecvThreadID);
		if (!m_hSendThread || !m_hRecvThread)
			return false;

		if (g_bSetTCPSocketThreadPriorities)
		{
			SetThreadPriority(m_hSendThread, THREAD_PRIORITY_LOWEST);
			SetThreadPriority(m_hRecvThread, THREAD_PRIORITY_LOWEST);
		}

		m_pHandler->Init(this);
		ResumeThread(m_hSendThread);
		ResumeThread(m_hRecvThread);

		return true;
	}

	void Term()
	{
		m_hExitThreadsEvent.SetEvent();
		if (m_hSendThread)
		{
			WaitForSingleObject(m_hSendThread, INFINITE);
			CloseHandle(m_hSendThread);
			m_hSendThread = nullptr;
		}
		if (m_hRecvThread)
		{
			WaitForSingleObject(m_hRecvThread, INFINITE);
			CloseHandle(m_hRecvThread);
			m_hRecvThread = nullptr;
		}
		m_hExitThreadsEvent.ResetEvent();

		if (m_Socket != INVALID_SOCKET)
		{
			closesocket(m_Socket);
			m_Socket = INVALID_SOCKET;
		}

		if (m_bDeleteHandler && m_pHandler)
		{
			m_pHandler->Release();
			m_pHandler = nullptr;
		}
	}

	void SetInitialSocketOptions()
	{
		BOOL val = TRUE;
		setsockopt(m_Socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&val, sizeof(BOOL));
		LINGER linger;
		linger.l_onoff = 1;
		linger.l_linger = 3;
		setsockopt(m_Socket, SOL_SOCKET, SO_LINGER, (char*)&linger, sizeof(linger));
	}

	bool InternalSend(void const* const* pChunks, const int* pChunkLengths, int nChunks, bool bPrependLength)
	{
		int totalLength = 0;
		for (int i = 0; i < nChunks; i++)
			totalLength += pChunkLengths[i];

		if (bPrependLength)
		{
			if (totalLength == 0)
				return true;
			totalLength += 4;
		}

		SendData_t* pSendData = (SendData_t*)malloc(sizeof(SendData_t) - 1 + totalLength);
		if (!pSendData)
			return false;
		pSendData->m_Len = totalLength;
		char* pOut = pSendData->m_Payload;
		if (bPrependLength)
		{
			*((int*)pOut) = totalLength - 4;
			pOut += 4;
		}
		for (int i = 0; i < nChunks; i++)
		{
			memcpy(pOut, pChunks[i], pChunkLengths[i]);
			pOut += pChunkLengths[i];
		}

		CVMPICriticalSectionLock csLock(&m_SendCS);
		csLock.Lock();
		m_SendDatas.AddToTail(pSendData);
		m_hReadyToSendEvent.SetEvent();
		csLock.Unlock();

		return true;
	}

	void SendThread_HandleTimeout()
	{
		CVMPICriticalSectionLock csLock(&m_SendCS);
		csLock.Lock();
		int count = m_SendDatas.Count();
		csLock.Unlock();
		if (count == 0)
		{
			void* pBuf[1] = { &g_KeepaliveSentinel };
			int len[1] = { sizeof(g_KeepaliveSentinel) };
			InternalSend(pBuf, len, 1, false);
		}
	}

	bool SendThread_HandleSendCompletionEvent()
	{
		Assert(m_bWaitingForSendCompletion);
		m_bWaitingForSendCompletion = false;
		DWORD cbTransfer, flags;
		if (!WSAGetOverlappedResult(m_Socket, &m_SendOverlapped, &cbTransfer, TRUE, &flags))
		{
			HandleError(WSAGetLastError());
			return false;
		}
		if (cbTransfer != m_nBytesToTransfer)
		{
			char str[512];
			Q_snprintf(str, sizeof(str), "Invalid # bytes transferred (%d) in send thread (should be %d)", cbTransfer, m_nBytesToTransfer);
			HandleError(ITCPSocketHandler::SocketError, str);
			return false;
		}
		CVMPICriticalSectionLock csLock(&m_SendCS);
		csLock.Lock();
		SendData_t* pSendData = m_SendDatas[m_SendDatas.Head()];
		free(pSendData);
		m_SendDatas.Remove(m_SendDatas.Head());
		m_bWaitingForSendCompletion = false;
		if (m_SendDatas.Count() > 0)
			m_hReadyToSendEvent.SetEvent();
		csLock.Unlock();
		return true;
	}

	bool SendThread_HandleReadyToSendEvent()
	{
		CVMPICriticalSectionLock csLock(&m_SendCS);
		csLock.Lock();
		Assert(!m_bWaitingForSendCompletion);
		SendData_t* pSendData = m_SendDatas[m_SendDatas.Head()];
		WSABUF buf = { pSendData->m_Len, pSendData->m_Payload };
		m_nBytesToTransfer = pSendData->m_Len;
		m_bWaitingForSendCompletion = true;
		csLock.Unlock();

		DWORD dwNumBytesSent = 0;
		DWORD ret = WSASend(m_Socket, &buf, 1, &dwNumBytesSent, 0, &m_SendOverlapped, NULL);
		DWORD err = WSAGetLastError();
		if (ret == 0 || (ret == SOCKET_ERROR && err == WSA_IO_PENDING))
		{
			return true;
		}
		else
		{
			HandleError(err);
			return false;
		}
	}

	DWORD SendThreadFn()
	{
		while (1)
		{
			HANDLE handles[] = {
				m_hExitThreadsEvent.GetEventHandle(),
				m_hSendCompletionEvent.GetEventHandle(),
				m_hReadyToSendEvent.GetEventHandle()
			};
			int nHandles = ARRAYSIZE(handles);
			if (m_bWaitingForSendCompletion)
				--nHandles;

			DWORD waitValue = WaitForMultipleObjects(nHandles, handles, FALSE, SEND_KEEPALIVE_INTERVAL);
			switch (waitValue)
			{
			case WAIT_TIMEOUT:
				if (g_bHandleTimeouts)
				{
					SendThread_HandleTimeout();
				}
				break;
			case WAIT_OBJECT_0:
				return 0;
			case WAIT_OBJECT_0 + 1:
				if (!SendThread_HandleSendCompletionEvent())
					return 1;
				break;
			case WAIT_OBJECT_0 + 2:
				if (!SendThread_HandleReadyToSendEvent())
					return 1;
				break;
			case WAIT_FAILED:
			{
				HandleError(GetLastError());
				return 1;
			}
			default:
			{
				char str[512];
				Q_snprintf(str, sizeof(str), "Unknown return value (%lu) from WaitForMultipleObjects", waitValue);
				HandleError(ITCPSocketHandler::SocketError, str);
				return 0;
			}
			}
		}
		return 0;
	}

	static DWORD WINAPI StaticSendThreadFn(LPVOID pParameter)
	{
		return ((CThreadedTCPSocket*)pParameter)->SendThreadFn();
	}

	//-------------------------------------------------------------------------------------------------
	// Receive thread functionality.
	//-------------------------------------------------------------------------------------------------
	bool RecvThread_WaitToReceiveSize()
	{
		return RecvThread_InternalRecv(&m_NextPacketLen, sizeof(m_NextPacketLen), false, true);
	}

	bool RecvThread_InternalHandleRecvCompletion(DWORD dwTransfer)
	{
		int cbTransfer = (int)dwTransfer;
		int nBytesWanted = m_nBytesToReceive - m_nBytesReceivedSoFar;
		if (cbTransfer > nBytesWanted)
		{
			char str[512];
			Q_snprintf(str, sizeof(str), "Invalid # bytes received (%d) in recv thread (should be %d)", cbTransfer, m_nBytesToReceive);
			HandleError(ITCPSocketHandler::SocketError, str);
			return false;
		}
		else if (cbTransfer < nBytesWanted)
		{
			m_nBytesReceivedSoFar += cbTransfer;
			char* pDest = (char*)&m_NextPacketLen;
			if (!m_bWaitingForSize)
			{
				Assert(m_pRecvBuffer);
				pDest = m_pRecvBuffer->m_Data;
			}
			return RecvThread_InternalRecv(&pDest[m_nBytesReceivedSoFar], m_nBytesToReceive - m_nBytesReceivedSoFar, true);
		}

		if (m_bWaitingForSize)
		{
			if (m_NextPacketLen == KEEPALIVE_SENTINEL)
			{
				return RecvThread_WaitToReceiveSize();
			}
			else
			{
				if (m_NextPacketLen < 1 || m_NextPacketLen > 1024 * 1024 * 75)
				{
					char str[512];
					Q_snprintf(str, sizeof(str), "Invalid packet size in RecvThread (size = %d)", m_NextPacketLen);
					HandleError(ITCPSocketHandler::SocketError, str);
					return false;
				}
				else
				{
					Assert(!m_pRecvBuffer);
					m_pRecvBuffer = (CTCPPacket*)malloc(sizeof(CTCPPacket) - 1 + m_NextPacketLen);
					m_pRecvBuffer->m_UserData = 0;
					m_pRecvBuffer->m_Len = m_NextPacketLen;
					return RecvThread_InternalRecv(m_pRecvBuffer->m_Data, m_pRecvBuffer->m_Len, false, false);
				}
			}
		}
		else
		{
			m_pHandler->OnPacketReceived(m_pRecvBuffer);
			m_pRecvBuffer = NULL;
			return RecvThread_WaitToReceiveSize();
		}
	}

	bool RecvThread_HandleRecvCompletionEvent()
	{
		DWORD cbTransfer, flags;
		if (!WSAGetOverlappedResult(m_Socket, &m_RecvOverlapped, &cbTransfer, TRUE, &flags))
		{
			HandleError(WSAGetLastError());
			return false;
		}
		return RecvThread_InternalHandleRecvCompletion(cbTransfer);
	}

	bool RecvThread_InternalRecv(void* pDest, int destSize, bool bContinuation, bool bWaitingForSize = false)
	{
		WSABUF buf = { destSize, (char*)pDest };

		if (!bContinuation)
		{
			m_bWaitingForSize = bWaitingForSize;
			m_nBytesToReceive = destSize;
			m_nBytesReceivedSoFar = 0;
		}

		DWORD dwFlags = 0;
		DWORD nBytesReceived = 0;
		DWORD ret = WSARecv(m_Socket, &buf, 1, &nBytesReceived, &dwFlags, &m_RecvOverlapped, NULL);
		DWORD dwLastError = WSAGetLastError();
		if (ret == 0 || (ret == SOCKET_ERROR && dwLastError == WSA_IO_PENDING))
			return true;
		else
		{
			HandleError(dwLastError);
			return false;
		}
	}

	DWORD RecvThreadFn()
	{
		if (!RecvThread_WaitToReceiveSize())
			return 1;

		HANDLE handles[] = { m_hExitThreadsEvent.GetEventHandle(), m_hRecvEvent.GetEventHandle() };

		while (1)
		{
			DWORD waitValue = WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, KEEPALIVE_TIMEOUT);
			switch (waitValue)
			{
			case WAIT_TIMEOUT:
				if (g_bHandleTimeouts)
				{
					HandleError(ITCPSocketHandler::ConnectionTimedOut, "Connection timed out");
					return 1;
				}
				break;
			case WAIT_OBJECT_0:
				return 0;
			case WAIT_OBJECT_0 + 1:
				if (!RecvThread_HandleRecvCompletionEvent())
					return 1;
				break;
			case WAIT_FAILED:
				HandleError(GetLastError());
				return 1;
			default:
			{
				char str[512];
				Q_snprintf(str, sizeof(str), "Unknown return value (%lu) from WaitForMultipleObjects", waitValue);
				HandleError(ITCPSocketHandler::SocketError, str);
				return 1;
			}
			}
		}

		return 0;
	}

	static DWORD WINAPI StaticRecvThreadFn(LPVOID pParameter)
	{
		return ((CThreadedTCPSocket*)pParameter)->RecvThreadFn();
	}

	//-------------------------------------------------------------------------------------------------
	// Error handling.
	//-------------------------------------------------------------------------------------------------
	bool CheckErrorSignal()
	{
		return m_bErrorSignal;
	}

	void HandleError(DWORD errorValue)
	{
		char* lpMsgBuf;
		FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL,
			GetLastError(),
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			(char*)&lpMsgBuf,
			0,
			NULL);
		int len = strlen(lpMsgBuf);
		while (len > 0 && (lpMsgBuf[len - 1] == '\n' || lpMsgBuf[len - 1] == '\r'))
		{
			--len;
			lpMsgBuf[len] = 0;
		}
		HandleError(ITCPSocketHandler::SocketError, lpMsgBuf);
		LocalFree(lpMsgBuf);
	}

	void HandleError(int errorCode, const char* pErrorString)
	{
		m_pHandler->OnError(errorCode, pErrorString);
		m_hExitThreadsEvent.SetEvent();
		m_bErrorSignal = true;
	}

private:
	//-------------------------------------------------------------------------------------------------
	// Data for the send thread.
	//-------------------------------------------------------------------------------------------------
	typedef struct
	{
		int m_Len;
		char m_Payload[1];
	} SendData_t;

	HANDLE m_hSendThread;
	WSAOVERLAPPED m_SendOverlapped;
	CEvent m_hReadyToSendEvent;
	CEvent m_hSendCompletionEvent;
	CVMPICriticalSection m_SendCS;
	DWORD m_nBytesToTransfer;
	bool m_bWaitingForSendCompletion;
	CUtlLinkedList<SendData_t*, int> m_SendDatas;

	//-------------------------------------------------------------------------------------------------
	// Data for the recv thread.
	//-------------------------------------------------------------------------------------------------
	HANDLE m_hRecvThread;
	int m_nBytesToReceive;
	int m_nBytesReceivedSoFar;
	bool m_bWaitingForSize;
	int m_NextPacketLen;
	WSAOVERLAPPED m_RecvOverlapped;
	CEvent m_hRecvEvent;
	CTCPPacket* m_pRecvBuffer;

	volatile bool m_bErrorSignal;
	CEvent m_hExitThreadsEvent;

	ITCPSocketHandler* m_pHandler;
	bool m_bDeleteHandler;

	SOCKET m_Socket;
	CIPAddr m_RemoteAddr;
};

class CTCPConnectSocket_Listener : public ITCPConnectSocket
{
public:
	CTCPConnectSocket_Listener() { m_Socket = INVALID_SOCKET; }
	virtual ~CTCPConnectSocket_Listener()
	{
		if (m_Socket != INVALID_SOCKET)
		{
			closesocket(m_Socket);
		}
	}

	static ITCPConnectSocket* Create(IHandlerCreator* pHandlerCreator, const unsigned short port, int nQueueLength)
	{
		CTCPConnectSocket_Listener* pRet = new CTCPConnectSocket_Listener;
		if (!pRet)
			return NULL;

		if (nQueueLength < 0)
		{
			Error("CTCPConnectSocket_Listener::Create - SOMAXCONN not allowed - causes some XP SP2 systems to stop receiving any network data (systemwide).");
		}

		CIPAddr addr(0, 0, 0, 0, port);
		pRet->m_Socket = TCPBind(&addr);
		if (pRet->m_Socket == INVALID_SOCKET ||
			listen(pRet->m_Socket, nQueueLength == -1 ? SOMAXCONN : nQueueLength) != 0)
		{
			pRet->Release();
			return false;
		}

		pRet->m_pHandler = pHandlerCreator;
		return pRet;
	}

public:
	virtual void Release() { delete this; }

	virtual bool Update(IThreadedTCPSocket** pSocket, unsigned long milliseconds)
	{
		*pSocket = NULL;
		if (m_Socket == INVALID_SOCKET)
			return false;

		fd_set readSet;
		readSet.fd_count = 1;
		readSet.fd_array[0] = m_Socket;
		TIMEVAL timeVal = { 0, milliseconds * 1000 };

		int status = select(0, &readSet, NULL, NULL, &timeVal);
		if (status > 0)
		{
			sockaddr_in addr;
			int addrSize = sizeof(addr);
			SOCKET newSock = accept(m_Socket, (struct sockaddr*)&addr, &addrSize);
			if (newSock == INVALID_SOCKET)
			{
				Assert(false);
				return true;
			}
			else
			{
				CIPAddr connectedAddr;
				SockAddrToIPAddr(&addr, &connectedAddr);
				IThreadedTCPSocket* pRet = CThreadedTCPSocket::Create(newSock, connectedAddr, m_pHandler->CreateNewHandler(), true);
				if (!pRet)
				{
					Assert(false);
					closesocket(m_Socket);
					m_Socket = INVALID_SOCKET;
					return false;
				}

				*pSocket = pRet;
				return true;
			}
		}
		else if (status == SOCKET_ERROR)
		{
			closesocket(m_Socket);
			m_Socket = INVALID_SOCKET;
			return false;
		}
		else
		{
			return true;
		}
	}

private:
	SOCKET m_Socket;
	IHandlerCreator* m_pHandler;
};

ITCPConnectSocket* ThreadedTCP_CreateListener(IHandlerCreator* pHandlerCreator, const unsigned short port, int nQueueLength)
{
	return CTCPConnectSocket_Listener::Create(pHandlerCreator, port, nQueueLength);
}

class CTCPConnectSocket_Connector : public ITCPConnectSocket
{
public:
	CTCPConnectSocket_Connector()
		: m_bConnected(false), m_Socket(INVALID_SOCKET), m_bError(false) {
	}
	virtual ~CTCPConnectSocket_Connector()
	{
		if (m_Socket != INVALID_SOCKET)
		{
			closesocket(m_Socket);
		}
	}

	static ITCPConnectSocket* Create(const CIPAddr& connectAddr, const CIPAddr& localAddr, IHandlerCreator* pHandlerCreator)
	{
		CTCPConnectSocket_Connector* pRet = new CTCPConnectSocket_Connector;

		pRet->m_Socket = TCPBind(&localAddr);
		if (pRet->m_Socket == INVALID_SOCKET)
		{
			pRet->Release();
			return NULL;
		}

		sockaddr_in addr;
		IPAddrToSockAddr(&connectAddr, &addr);

		DWORD val = 1;
		int status = ioctlsocket(pRet->m_Socket, FIONBIO, &val);
		if (status != 0)
		{
			Assert(false);
			pRet->Release();
			return NULL;
		}

		pRet->m_RemoteAddr = connectAddr;
		pRet->m_pHandlerCreator = pHandlerCreator;

		int ret = connect(pRet->m_Socket, (struct sockaddr*)&addr, sizeof(addr));
		if (ret == 0)
		{
			pRet->m_bConnected = true;
			return pRet;
		}
		else if (ret == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
		{
			return pRet;
		}
		else
		{
			Assert(false);
			pRet->Release();
			return NULL;
		}
	}

public:
	virtual void Release() { delete this; }

	virtual bool Update(IThreadedTCPSocket** pSocket, unsigned long milliseconds)
	{
		*pSocket = NULL;
		if (m_bError)
			return false;

		if (m_Socket == INVALID_SOCKET)
			return true;

		if (!m_bConnected)
		{
			TIMEVAL timeVal = { 0, milliseconds * 1000 };
			fd_set writeSet;
			writeSet.fd_count = 1;
			writeSet.fd_array[0] = m_Socket;
			int ret = select(0, NULL, &writeSet, NULL, &timeVal);
			if (ret > 0)
			{
				m_bConnected = true;
			}
			else if (ret == SOCKET_ERROR)
			{
				return EnterErrorMode();
			}
		}

		if (m_bConnected)
		{
			DWORD val = 0;
			int status = ioctlsocket(m_Socket, FIONBIO, &val);
			if (status != 0)
			{
				Assert(false);
				m_bError = true;
				closesocket(m_Socket);
				m_Socket = INVALID_SOCKET;
				return false;
			}

			IThreadedTCPSocket* pRet = CThreadedTCPSocket::Create(m_Socket, m_RemoteAddr, m_pHandlerCreator->CreateNewHandler(), true);
			if (pRet)
			{
				m_Socket = INVALID_SOCKET;
				*pSocket = pRet;
				return true;
			}
			else
			{
				return EnterErrorMode();
			}
		}
		else
		{
			return true;
		}
	}

	bool EnterErrorMode()
	{
		Assert(false);
		m_bError = true;
		closesocket(m_Socket);
		m_Socket = INVALID_SOCKET;
		return false;
	}

private:
	bool m_bError;
	bool m_bConnected;
	SOCKET m_Socket;
	CIPAddr m_RemoteAddr;
	IHandlerCreator* m_pHandlerCreator;
};

ITCPConnectSocket* ThreadedTCP_CreateConnector(const CIPAddr& addr, const CIPAddr& localAddr, IHandlerCreator* pHandlerCreator)
{
	return CTCPConnectSocket_Connector::Create(addr, localAddr, pHandlerCreator);
}

void ThreadedTCP_EnableTimeouts(bool bEnable)
{
	g_bHandleTimeouts = bEnable;
}

void ThreadedTCP_SetTCPSocketThreadPriorities(bool bSetTCPSocketThreadPriorities)
{
	g_bSetTCPSocketThreadPriorities = bSetTCPSocketThreadPriorities;
}
