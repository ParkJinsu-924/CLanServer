#include "CLanServer.h"

extern HANDLE hEvent;

UINT WINAPI CLanServer::StaticWorkerThread(PVOID p)
{
	CLanServer* pObj = (CLanServer*)p;

	pObj->WorkerThread();

	return 0;
}

UINT WINAPI CLanServer::StaticAcceptThread(PVOID p)
{
	CLanServer* pObj = (CLanServer*)p;
	
	pObj->AcceptThread();

	return 0;
}

VOID CLanServer::WorkerThread()
{
	for (;;)
	{
		stSESSION* pSession = nullptr;
		OVERLAPPED* pOverlapped = nullptr;
		DWORD transferredBytes = 0;
		BYTE abortedFlag = 0;

		GetQueuedCompletionStatus(mIocpHandle, &transferredBytes, (PULONG_PTR)&pSession, &pOverlapped, INFINITE);

		if (pOverlapped->Internal == ERROR_OPERATION_ABORTED)
		{
			abortedFlag = 1;
		}

		OnWorkerThreadBegin(pSession->sessionID, transferredBytes, abortedFlag);

		//워커스레드 종료, 포스트큐 호출 시 & 갯큐드컴플리션 함수 실패 시
		if (pOverlapped == nullptr)
		{
			PostQueuedCompletionStatus(mIocpHandle, NULL, NULL, NULL);
			break;
		}

		//CancelIo로 인해 IO가 전부 종료되었을 경우
		if (transferredBytes == 0 || pOverlapped->Internal == ERROR_OPERATION_ABORTED)
		{
			goto IOCOUNT_DEQ;
		}

		if (pOverlapped == &pSession->recvOverlapped)
		{
			RecvProc(transferredBytes, pSession);
		}
		else if (pOverlapped == &pSession->sendOverlapped)
		{
			SendProc(pSession);
		}

	IOCOUNT_DEQ:
		if (InterlockedDecrement16(&pSession->ioRelease.IOCount) == 0)
		{
			ReleaseSession(pSession);
		}
		OnWorkerThreadEnd();
	}
	return;
}

//VOID CLanServer::WorkerThread()
//{
//	for (;;)
//	{
//		stSESSION* pSession = nullptr;
//		OVERLAPPED* pOverlapped = nullptr;
//		DWORD transferredBytes = 0;
//
//		OVERLAPPED_ENTRY entry[50];
//		ULONG removed;
//		GetQueuedCompletionStatusEx(mIocpHandle, entry, _countof(entry), &removed, INFINITE, FALSE);
//		
//		for (int i = 0; i < removed; i++)
//		{
//			pOverlapped = entry[i].lpOverlapped;
//			if (pOverlapped == nullptr)
//			{
//				PostQueuedCompletionStatus(mIocpHandle, NULL, NULL, NULL);
//				break;
//			}
//
//			pSession = (stSESSION*)entry[i].lpCompletionKey;
//			transferredBytes = entry[i].dwNumberOfBytesTransferred;
//
//			if (transferredBytes == 0 || pOverlapped->Internal == ERROR_OPERATION_ABORTED)
//			{
//				goto IOCOUNT_DEQ;
//			}
//
//			if (pOverlapped == &pSession->recvOverlapped)
//			{
//				RecvProc(transferredBytes, pSession);
//			}
//			else if (pOverlapped == &pSession->sendOverlapped)
//			{
//				SendProc(pSession);
//			}
//
//		IOCOUNT_DEQ:
//			if (InterlockedDecrement16(&pSession->ioRelease.IOCount) == 0)
//			{
//				ReleaseSession(pSession);
//			}
//		}
//	}
//	return;
//}

VOID CLanServer::RecvProc(SHORT transferredBytes, stSESSION* pSession)
{
	pSession->recvQ.MoveRear(transferredBytes);

	for (;;)
	{
		stHEADER header;

		INT useSize = pSession->recvQ.GetUseSize();

		//네트워크 헤더의 크기보다 작을 경우 break타고 다시 대기
		if (useSize < sizeof(header))
			break;

		//Peek로 헤더 뽑고
		pSession->recvQ.Peek((CHAR*)&header, sizeof(header));

		if (header.length >= QUEUE_SIZE)
		{
			Disconnect(pSession->sessionID);
			break;
		}
		//뽑은 헤더를 통해서 패킷이 전부 도착했는지 확인하기
		if (useSize - sizeof(header) < header.length)
			break;

		//직렬화버퍼 Alloc하기
		CSerializationBuffer* packet = CSerializationBuffer::Alloc();

		//직렬화버퍼에 패킷을 전부 뽑기
		pSession->recvQ.Dequeue(packet->GetBufferPtr(), header.length + sizeof(header));

		//디큐하여 뽑았으니 WritePos를 옮겨준다.
		packet->MoveWritePos(header.length);

		//OnRecv호출을 통해 컨텐츠부분에 패킷 전달하기
		OnRecv(pSession->sessionID, packet);

		//작업 완료 후 직렬화버퍼의 refCount를 감소하기
		packet->DeqRef();
	}

	InterlockedIncrement64((LONG64*)&recvTPS);
	RecvPost(pSession);
}

VOID CLanServer::SendProc(stSESSION* pSession)
{
	for (INT i = 0; i < pSession->sendCount; i++)
		pSession->sendPacketPtrBuf[i]->DeqRef();

	InterlockedExchange((LONG*)&pSession->sendCount, 0);
	InterlockedExchange8((CHAR*)&pSession->sendFlag, FALSE);

	if (pSession->sendQ.GetQueueSize() > 0)
	{
		SendPost(pSession);
	}

	OnSend(pSession->sessionID);
}



VOID CLanServer::AcceptThread()
{
	for (;;)
	{
		SOCKADDR_IN clientAddr;
		
		INT size = sizeof(clientAddr);

		SOCKET clientSocket = accept(mListenSocket, (SOCKADDR*)&clientAddr, &size);
		
		SetEvent(hEvent);

		acceptCount++;

		acceptTPS++;

		if (clientSocket == INVALID_SOCKET)
		{
			INT errCode = WSAGetLastError();

			//원격지에서 종료되었음
			if (errCode == WSAECONNRESET)
				continue;
			else
			{
				break;
			}
		}

		WCHAR clientIP[46];
		
		InetNtop(AF_INET, (const VOID*)&clientAddr.sin_addr.s_addr, clientIP, sizeof(clientIP));

		BYTE acceptFlag = OnConnectionRequest(clientIP, clientAddr.sin_port);
		if (!acceptFlag)
		{
			continue;
		}

		if (mCurrentClientCount == mMaxClientNum)
		{
			closesocket(clientSocket);
			continue;
		}

		InterlockedIncrement(&mCurrentClientCount);

		INT sessionArrayIndex;
		mSessionArrayIndexStack.Pop(&sessionArrayIndex);

		stSESSION* pSession = &mSessionArray[sessionArrayIndex];


		//IO카운트를 올려주는 이유 : OnClientJoin에서 Send를 하는데 Send를 함과 동시에 완료통지가 떨어지고
		//IO카운트를 Decrement하는데까지 간다면, 세션이 Release가 될수도 있다.
		//AcquireSession에서의 동기화 문제로 인해 이곳에서 해주어야 한다.
		InterlockedIncrement16(&pSession->ioRelease.IOCount);
		pSession->socket = clientSocket;
		pSession->socketForRelease = clientSocket;
		pSession->sessionID = MakeSessionID(sessionArrayIndex, mClientID++);
		pSession->sendFlag = FALSE;
		pSession->ioRelease.releaseFlag = 0;
		pSession->sessionArrayIndex = sessionArrayIndex;
		pSession->sendCount = 0;
		pSession->recvQ.ClearBuffer();

		CreateIoCompletionPort((HANDLE)clientSocket, mIocpHandle, (ULONG_PTR)&mSessionArray[sessionArrayIndex], NULL);

		OnClientJoin(mSessionArray[sessionArrayIndex].sessionID);

		RecvPost(&mSessionArray[sessionArrayIndex]);

		if (InterlockedDecrement16(&mSessionArray[sessionArrayIndex].ioRelease.IOCount) == 0)
		{
			ReleaseSession(&mSessionArray[sessionArrayIndex]);
		}
	}
	return;
}

VOID CLanServer::RecvPost(stSESSION* pSession)
{
	DWORD recvQFreeSize = pSession->recvQ.GetFreeSize();
	DWORD recvQDirectEnqueueSize = pSession->recvQ.GetDirectEnqueueSize();

	INT wsaBufCount = 1;
	WSABUF dataBuf[2];

	dataBuf[0].buf = pSession->recvQ.GetRearBufferPtr();
	dataBuf[0].len = recvQDirectEnqueueSize;

	if (recvQFreeSize > recvQDirectEnqueueSize)
	{
		wsaBufCount++;
		dataBuf[1].buf = pSession->recvQ.GetStartBufferPtr();
		dataBuf[1].len = recvQFreeSize - recvQDirectEnqueueSize;
	}

	DWORD flags = 0;
	ZeroMemory(&pSession->recvOverlapped, sizeof(pSession->recvOverlapped));
	InterlockedIncrement16(&pSession->ioRelease.IOCount);
	INT retval = WSARecv(pSession->socket, dataBuf, wsaBufCount, NULL, &flags, &pSession->recvOverlapped, NULL);
	if (retval == SOCKET_ERROR)
	{
		if (WSAGetLastError() == WSA_IO_PENDING)
		{
			//WSA_IO_PENDING이라면
			//이미 Recv가 걸려있다. Disconnect가 WSARecv가 걸려있는 이후
			//걸렸다면 Recv를 해제시켜줘야한다.
			if (pSession->socket == INVALID_SOCKET)
			{
				//I/O카운트 감소는 ABORTED에서 진행해 준다.
				CancelIoEx((HANDLE)pSession->socketForRelease, NULL);
			}
		}
		else
		{
			if (InterlockedDecrement16(&pSession->ioRelease.IOCount) == 0)
			{
				ReleaseSession(pSession);
			}
		}
	}
}

BYTE CLanServer::SendPacket(UINT64 sessionID, CSerializationBuffer* packet)
{
	stHEADER header;
	INT enqueueSize;
	stSESSION* pSession;

	pSession = FindSession(sessionID);
	if (pSession == nullptr)
		return FALSE;

	if (!AcquireSession(sessionID, pSession))
		return FALSE;

	header.length = packet->GetContentUseSize();
	packet->PutNetworkHeader((CHAR*)&header, sizeof(header));

	packet->AddRef();

	InterlockedIncrement64((LONG64*)&sendTPS);

	pSession->sendQ.Enqueue(packet);

	SendPost(pSession);
	//useSize가 0이 나오는 상황을 고려해 SendPost를 한 번 더 호출하고있다.
	if (pSession->sendQ.GetQueueSize() > 0)
	{
		SendPost(pSession);
	}

	/*//이 조치가 있는 이유는, 만약 끊어진 세션이 있고 IO완료 통지에서 마지막 IO카운트 감소부분을 타려는 찰나에 SendPacket을 호출하였고 IO카운트를 증가시켜버렸다.
	//이럴 경우, IO완료 통지에서는 IO가 0이 됨을 감지하지 못하고 ReleaseSession을 타지 못한다. 이럴 경우를 대비하여 SendPacket에서라도 ReleaseSession을 호출시켜줘야만한다.
	if (InterlockedDecrement16(&pSession->ioRelease.IOCount) == 0)
	{
		ReleaseSession(pSession);
	}*/

	LeaveSession(pSession);
	
	return TRUE;
}

//SendPost의 분석
//SendPost함수는 모든스레드 통틀어서 단 한 스레드에서만 실행될 수 있다.
//SendPost의 useSize가 0이 되는 상황이 올 수 있다.
// --> recv완료 포스트에서 보낼 데이터를 인큐를 한다.
// --> 인큐이후에 스레드가 ready큐에 들어간다.
// --> 이 때, 다른 send완료 통지에서 해당 transferred만큼 Dequeue하고\
//		그 다음에 있는 if(GetUseSize()) sendPost();를 호출한다.
// --> 이 때, SendPost의 완료통지가 또 온다. 그럼 해당 transferred가 Dequeue된다.
// --> 그럼 다른 스레드가 Enqueue하지 않았다면, 사이즈가 0이 될것이다.
// --> 다시 ready큐에있던 스레드가 깨어난다.
// --> 일을 수행하고 SendPost를 호출한다. 그럼 SendPost안에있는 useSize가 0이 나온다.
//SendPost에서 useSize가 0이 나왔을 경우의 대처
// --> 상황이 발생하는 배경 :\
		useSize가 0이 된다.\
		useSize를 구하는 코드와 인터락의 코드 사이에서 누군가가 인큐를 한다.\
		다른 스레드는 인큐를 하고 SendPost를 호출한다.\
		근데 아직 sendFlag가 true이기때문에 SendPost의 함수안으로 들어오지못한다.\
		즉 아무것도 하지못하고 밖으로 나와버린다.\
		이 때 다시 돌아와 인터락을 실행하고 반환한다.\
		이렇게 되면 결국 다른 누군가가 인큐한 데이터는 전송되지 못하고 남게된다.\
// --> 첫번째로 goto를 쓰는 방법이 있다.
// --> 내가 한 방법은 SendPost이후 GetUseSize를 한 번 더 체크한 뒤 데이터가 남아있다면 SendPost 를 다시 호출한다.
VOID CLanServer::SendPost(stSESSION* pSession)
{
	CHAR retval = InterlockedExchange8((CHAR*)&pSession->sendFlag, TRUE);

	if (retval == TRUE)
	{
		return;
	}

	auto pSendQ = &pSession->sendQ;

	INT useSize = pSendQ->GetQueueSize();

	//{																				//상황이 발생하는 배경에서 말하는 useSize구하는 코드와 인터락 코드의 사이
	if (useSize == 0)
	{
	//}																				//요기까지
		InterlockedExchange8((CHAR*)&pSession->sendFlag, FALSE);
		return;
	}

	WSABUF dataBuf[MAXWSABUF];

	for (INT i = 0; i < useSize; i++)
	{
		pSendQ->Dequeue(&pSession->sendPacketPtrBuf[i]);
		dataBuf[i].buf = pSession->sendPacketPtrBuf[i]->GetBufferPtr();
		dataBuf[i].len = pSession->sendPacketPtrBuf[i]->GetTotalUseSize();
	}
	pSession->sendCount = useSize;

	DWORD flags = 0;

	InterlockedIncrement16(&pSession->ioRelease.IOCount);

	ZeroMemory(&pSession->sendOverlapped, sizeof(OVERLAPPED));
	INT sendRetval = WSASend(pSession->socket, dataBuf, useSize, NULL, flags, &pSession->sendOverlapped, NULL);
	if (sendRetval == SOCKET_ERROR)
	{
		if (WSAGetLastError() != WSA_IO_PENDING)
		{

			// 소켓 연결 끊김
			if (InterlockedDecrement16(&pSession->ioRelease.IOCount) == 0)
			{
				ReleaseSession(pSession);
			}
		}
		else
		{
			if (pSession->socket == INVALID_SOCKET)
			{
				CancelIoEx((HANDLE)pSession->socketForRelease, NULL);
			}
		}
	}
}

CLanServer::stSESSION* CLanServer::FindSession(UINT64 sessionID)
{
	USHORT index = (USHORT)sessionID;
	
	//한번 더 최종 비교
	if (mSessionArray[index].sessionID == sessionID)
		return &mSessionArray[index];
	else
		return nullptr;
}

UINT64 CLanServer::MakeSessionID(USHORT sessionArrayIndex, UINT64 sessionUniqueID)
{
	UINT64 sessionID = 0;

	sessionID = sessionUniqueID;

	sessionID = sessionID << 16;

	sessionID += sessionArrayIndex;

	return sessionID;
}

BYTE CLanServer::ReleaseSession(stSESSION* pSession)
{
	stIO_RELEASE exchangeStruct;
	exchangeStruct.IOCount = 0;
	exchangeStruct.releaseFlag = 1;

	INT* pCompareVal = (INT*)&exchangeStruct;

	//Release가 된걸로(exchangeStruct) 비교하고 만약 release가 됬다면 초기값이 0이 아닐것이기때문에
	//반복문을 탈것이고, release가 안됬다면 releaseFlag = 0이고 iocount도 0이기때문에 0이 나온다.
	//즉, 0 != Interlocked라면 Release된거, 0 == Interlocked라면 Release안된거
	if (0 != InterlockedCompareExchange((LONG*)&pSession->ioRelease, (LONG)*pCompareVal, (LONG)0))
	{
		//다른 어딘가에서 Release가 되었다. 또 할 필요가 없다.
		return FALSE;
	}

	closesocket(pSession->socketForRelease);

	OnClientLeave(pSession->sessionID);

	for (INT i = 0; i < pSession->sendCount; i++)
	{
		pSession->sendPacketPtrBuf[i]->DeqRef();
	}

	for (;;)
	{
		CSerializationBuffer* pPacket;
		if (!pSession->sendQ.Dequeue(&pPacket))
		{
			break;
		}
		pPacket->DeqRef();
	}

	InterlockedDecrement(&mCurrentClientCount);

	mSessionArrayIndexStack.Push(pSession->sessionArrayIndex);
	
	return TRUE;
}

BYTE CLanServer::AcquireSession(UINT64 sessionID, stSESSION* pSession)
{
	//IOCount를 증가시키는 순간 세션은 Release가 될일이 없음
	//InterlockedIncrement를 통해 증가했을 때 0 -> 1 로 증가했다고 가정할 경우, Release된 세션임을 판단하면
	//안되는 이유 = 여러 스레드에서 SendPacket을 동시에 호출했을 경우에 1이 아닌 2, 3, 4, 5 그 이상이 될 수 있다.
	//멀티스레드에서는 의미가 없다.
	//InterlockedIncrement16(&pSession->ioRelease.IOCount);

	//허나 만약에 내가 알던 sessionID랑 포인터로 접근한 세션아이디랑 다르다면 세션이 다른것임
	//if (pSession->sessionID != sessionID)
	//{
		//IOCount가 0이고 Release가 된 세션이 아니라면
		//if (InterlockedDecrement16(&pSession->ioRelease.IOCount) == 0 && pSession->ioRelease.releaseFlag == 0)
		//{
			//내가 올려놓음으로써 세션이 해제가 안됬을수도있기때문에 차감과동시에 0체크후 Release까지
			//ReleaseSession(pSession);
		//}
		//return false;
	//}

	InterlockedIncrement16(&pSession->ioRelease.IOCount);
	

	if (pSession->ioRelease.releaseFlag == TRUE || pSession->sessionID != sessionID)
	{
		LeaveSession(pSession);
		
		return FALSE;
	}

	return TRUE;
}

void CLanServer::LeaveSession(stSESSION* pSession)
{
	if (InterlockedDecrement16(&pSession->ioRelease.IOCount) == 0)
	{
		ReleaseSession(pSession);
	}
}

CLanServer::CLanServer()
	: mListenSocket(INVALID_SOCKET)
	, mServerPort(NULL)
	, mMaxClientNum(NULL)
	, mCurrentClientCount(NULL)
	, mWorkerThreadRunNum(NULL)
	, mWorkerThreadCreateNum(NULL)
	, mClientID(NULL)
	, mIocpHandle(INVALID_HANDLE_VALUE)
	, mThreadArr(nullptr)
	, mSessionArray(nullptr) {}

BYTE CLanServer::Start(const WCHAR* ip, SHORT port, INT workerThreadRunNum, INT workerThreadCreateNum, BYTE nagleOption, INT maxClientNum)
{
	INT retval;

	WSADATA wsa;

	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		return FALSE;
	}

	mListenSocket = socket(AF_INET, SOCK_STREAM, NULL);
	
	if (mListenSocket == INVALID_SOCKET)
	{
		return FALSE;
	}

	SOCKADDR_IN serverAddr;
	serverAddr.sin_family = AF_INET;
	InetPton(AF_INET, ip, &serverAddr.sin_addr);
	serverAddr.sin_port = htons(port);

	retval = bind(mListenSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr));
	if (retval == SOCKET_ERROR)
	{
		return FALSE;
	}

	//네이글 옵션
	BYTE optval = nagleOption;
	retval = setsockopt(mListenSocket, IPPROTO_TCP, TCP_NODELAY, (const CHAR*)&optval, sizeof(optval));
	if (retval == SOCKET_ERROR)
	{
		return FALSE;
	}

	//RST 설정
	linger lingerOpt;
	lingerOpt.l_onoff = 1;  // 링거옵션 1 : on, 0 : off
	lingerOpt.l_linger = 0; // RST 보내기 위해 0으로 세팅

	retval = setsockopt(mListenSocket, SOL_SOCKET, SO_LINGER, (const CHAR*)&lingerOpt, sizeof(lingerOpt));
	if (retval == SOCKET_ERROR)
	{
		printf("%d\n", WSAGetLastError());
		return FALSE;
	}

	//송신버퍼 0설정
	//INT bufSize = 0;
	//socklen_t len = sizeof(bufSize);
	//setsockopt(mListenSocket, SOL_SOCKET, SO_SNDBUF, (const char*)&bufSize, sizeof(bufSize));

	mIocpHandle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, workerThreadRunNum);
	if (mIocpHandle == NULL)
	{
		return FALSE;
	}
	
	mThreadArr = new HANDLE[workerThreadCreateNum + 1];

	mMaxClientNum = maxClientNum;

	mSessionArray = new stSESSION[maxClientNum];

	//인덱스들을 쭉 담는다.
	for (INT sessionCount = 0; sessionCount < maxClientNum; sessionCount++)
	{
		mSessionArrayIndexStack.Push(sessionCount);
	}

	retval = listen(mListenSocket, SOMAXCONN);
	if (retval == SOCKET_ERROR)
	{
		return FALSE;
	}

	INT threadCount = 0;

	for (threadCount = 0; threadCount < workerThreadCreateNum; threadCount++)
	{
		mThreadArr[threadCount] = (HANDLE)_beginthreadex(NULL, NULL, StaticWorkerThread, (PVOID)this, NULL, NULL);
	}

	mThreadArr[threadCount] = (HANDLE)_beginthreadex(NULL, NULL, StaticAcceptThread, (PVOID)this, NULL, NULL);

	return true;
}

VOID CLanServer::Stop()
{
	//새로운 접속자를 막기위해 리슨소켓 닫기, 여기서 AcceptThread가 종료하게 된다.
	closesocket(mListenSocket);

	for (int clientNum = 0; clientNum < mMaxClientNum; clientNum++)
	{
		Disconnect(mSessionArray[clientNum].sessionID);
	}

	for (;;)
	{
		//세션이 전부 반납됨
		if (mSessionArrayIndexStack.GetUseSize() == mMaxClientNum)
			break;

		Sleep(100);
	}

	//WorkerThread가 연쇄반응을 통해 꺼지게 될거임
	PostQueuedCompletionStatus(mIocpHandle, NULL, NULL, NULL);

	std::list<HANDLE> notTerminatedThreadList;

	//mWorkerThreadCreateNum + 1 = 워커스레드갯수 + 억셉트스레드
	for (int threadIndex = 0; threadIndex < mWorkerThreadCreateNum + 1; threadIndex++)
	{
		DWORD retval = WaitForSingleObject(mThreadArr[threadIndex], 1000);
		if (retval == WAIT_TIMEOUT)
		{
			notTerminatedThreadList.push_back(mThreadArr[threadIndex]);
		}
	}

	for (auto listItor = notTerminatedThreadList.begin(); listItor != notTerminatedThreadList.end(); ++listItor)
	{
		DWORD retval = WaitForSingleObject(*listItor, 0);
		if (retval == WAIT_TIMEOUT)
		{
			TerminateThread(*listItor, 1);
		}
	}

	//쓰레드배열 해제
	delete mThreadArr;
	//세션배열 해제
	delete[] mSessionArray;
}

INT CLanServer::GetClientCount()
{
	return mCurrentClientCount;
}

UINT64 CLanServer::GetAcceptCount()
{
	return acceptCount;
}

BYTE CLanServer::Disconnect(UINT64 sessionID)
{

	stSESSION* pSession;

	pSession = FindSession(sessionID);
	if (pSession == nullptr)
	{
		return FALSE;
	}

	if (!AcquireSession(sessionID, pSession))
	{
		return FALSE;
	}
	
	if (InterlockedExchange64((LONG64*)&pSession->socket, INVALID_SOCKET) == INVALID_SOCKET)
	{
		LeaveSession(pSession);
		return FALSE;
	}

	CancelIoEx((HANDLE)pSession->socketForRelease, NULL);

	LeaveSession(pSession);

	return TRUE;
}

VOID CLanServer::OnSend(UINT64) {}

VOID CLanServer::OnWorkerThreadBegin(UINT64, DWORD, BYTE) {}

VOID CLanServer::OnWorkerThreadEnd() {}
