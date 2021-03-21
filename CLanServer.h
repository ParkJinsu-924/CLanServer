#pragma once
#define _CRT_SECURE_NO_WARNINGS
#pragma comment(lib, "ws2_32.lib")
#include<WinSock2.h>
#include<WS2tcpip.h>
#include <Windows.h>
#include <iostream>
#include <process.h>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <stack>
#include <list>

#include "CRingBuffer\CRingBuffer.h"
#include "CSerializationBufferLan\CSerializationBufferLan.h"
#include "CLockFreeQueue\LockFreeQueue.h"
#include "CLockFreeStack\LockFreeStack.h"
#include "CPerformanceProfiler/PerformanceProfiler.h"

#define MAXWSABUF 200

class CLanServer
{
public:
	CLanServer();

	BYTE Start(const WCHAR* openIP, SHORT port, INT workerThreadRunNum, INT workerThreadCreateNum, BYTE nagleOption, INT maxClientNum);
	
	BYTE SendPacket(UINT64, CSerializationBuffer*);
	
	BYTE Disconnect(UINT64);
	
	INT GetClientCount();
	
	UINT64 GetAcceptCount();
	
	VOID Stop();

	//Accept후 접속처리 완료 후 호출
	virtual VOID OnClientJoin(UINT64) = 0;

	//Release후 호출
	virtual VOID OnClientLeave(UINT64) = 0;

	//accept직후, return false, return true;
	//false 시 클라이언트 거부, true 시 접속 허용
	virtual bool OnConnectionRequest(PWCHAR, SHORT) = 0;

	//패킷 수신 완료 후
	virtual VOID OnRecv(UINT64, CSerializationBuffer*) = 0;

	//패킷 송신 완료 후
	virtual VOID OnSend(UINT64);
	//
	//워커스레드 GQCS 바로 하단에서 호출
	virtual VOID OnWorkerThreadBegin(UINT64, DWORD, BYTE);
	//
	//워커스레드 1루프 종료 후
	virtual VOID OnWorkerThreadEnd();
	
	UINT64 acceptCount = 0;
	UINT64 acceptTPS = 0;
	UINT64 recvTPS = 0;
	UINT64 sendTPS = 0;

private:
	struct stIO_RELEASE
	{
		SHORT IOCount;
		SHORT releaseFlag;
	};


	__declspec(align(64))struct stSESSION
	{
		UINT64 sessionID;
		SOCKET socket;
		SOCKET socketForRelease;
		INT sessionArrayIndex;

		__declspec(align(64))OVERLAPPED recvOverlapped;
		OVERLAPPED sendOverlapped;
		BYTE sendFlag;
		INT sendCount;
		stIO_RELEASE ioRelease;
		
		CRingBuffer recvQ;
		LockFreeQueue<CSerializationBuffer*> sendQ;
		CSerializationBuffer* sendPacketPtrBuf[MAXWSABUF];

	};
#pragma pack(1)
	struct stHEADER
	{
		USHORT length;
	};
#pragma pack()

private:
	SOCKET mListenSocket;
	UINT64 mCurrentClientCount;
	UINT64 mClientID;
	INT mMaxClientNum;
	INT mWorkerThreadRunNum;
	INT mWorkerThreadCreateNum;
	SHORT mServerPort;
	HANDLE mIocpHandle;
	HANDLE* mThreadArr;

	LockFreeStack<INT> mSessionArrayIndexStack;
	stSESSION* mSessionArray;

	static UINT WINAPI StaticWorkerThread(PVOID p);
	VOID WorkerThread();

	static UINT WINAPI StaticAcceptThread(PVOID p);
	VOID AcceptThread();
	
	VOID RecvProc(SHORT, stSESSION*);
	VOID SendProc(stSESSION*);

	VOID RecvPost(stSESSION*);
	VOID SendPost(stSESSION*);

	stSESSION* FindSession(UINT64);
	UINT64 MakeSessionID(USHORT, UINT64);

	BYTE ReleaseSession(stSESSION*);

	BYTE AcquireSession(UINT64, stSESSION*);

	VOID LeaveSession(stSESSION*);
};
