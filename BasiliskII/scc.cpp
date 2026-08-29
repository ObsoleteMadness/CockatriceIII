/*
 *  scc.cpp - Z8530 SCC emulation with LocalTalk-over-UDP (LToUDP)
 *
 *  Adapted for Cockatrice III from Mini vMac (SCCEMDEV.c & LTOVRUDP.h)
 *  and Snow (scc.rs & localtalk_bridge.rs)
 *  Copyright (C) 2004 Philip Cummins, Paul C. Pratt
 *  Copyright (C) 2011-2012 Michael Fort, Rob Mitchelmore, Weston Pawlowski
 *  Copyright (C) 2023-2024 Thomas van der Velden
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "prefs.h"
#include "scc.h"

#include "m68k.h"
#include "newcpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <process.h>
typedef int socklen_t;
#define my_closesocket closesocket
#define my_INVALID_SOCKET INVALID_SOCKET
#define my_SOCKET SOCKET
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#define my_closesocket close
#define my_INVALID_SOCKET (-1)
#define my_SOCKET int
#endif

#define Bit0 1
#define Bit1 2
#define Bit2 4
#define Bit3 8
#define Bit4 16
#define Bit5 32
#define Bit6 64
#define Bit7 128

/* SCC Interrupt types */
#define SCC_A_Rx       8 /* Rx Char Available */
#define SCC_A_Rx_Spec  7 /* Rx Special Condition */
#define SCC_A_Tx_Empty 6 /* Tx Buffer Empty */
#define SCC_A_Ext      5 /* External/Status Change */
#define SCC_B_Rx       4 /* Rx Char Available */
#define SCC_B_Rx_Spec  3 /* Rx Special Condition */
#define SCC_B_Tx_Empty 2 /* Tx Buffer Empty */
#define SCC_B_Ext      1 /* External/Status Change */

#define LT_TxBfMxSz 1800
#define RX_BUFFER_ALLOCATION 1800

#ifndef SCC_DEBUG
#define SCC_DEBUG 0
#endif

#if SCC_DEBUG
#define SCC_LOG(fmt, ...) printf("SCC: " fmt, ##__VA_ARGS__)
#else
#define SCC_LOG(fmt, ...) ((void)0)
#endif

typedef struct {
	bool TxEnable;
	bool RxEnable;
	bool TxIE;          /* Transmit Interrupt Enable */
	bool TxUnderrun;
	bool SyncHunt;
	bool TxIP;          /* Transmit Interrupt Pending */
	bool ExtIP;         /* External/Status Interrupt Pending */
	uint32 RxBuff;
	bool TxBufferEmpty;
	bool ExtIE;
	bool AddrSrchMd;
	uint32 RxIntMode;
	bool FirstChar;
	uint32 SyncMode;
	bool RxChrAvail;
	bool EndOfFrame;
	bool BreakAbort;
	bool SendBreak;
	uint32 BaudLo;
	uint32 BaudHi;
	uint32 WR15;
} Channel_Ty;

typedef struct {
	Channel_Ty a[2];    /* 0 = channel A (Modem), 1 = channel B (Printer) */
	int SCC_Interrupt_Type;
	int PointerBits;
	uint8 InterruptVector;
	bool MIE;           /* Master Interrupt Enable */
} SCC_Ty;

static SCC_Ty SCC;
uint8 SCCInterruptRequest = 0;
static bool scc_initialized = false;
static bool ltoudp_active = false;

/*
 *  LocalTalk over UDP (LToUDP) implementation
 */

static uint8 tx_buffer[4 + LT_TxBfMxSz] = {'p', 'p', 'p', 'p'};
static uint8 *LT_TxBuffer = &tx_buffer[4];
static size_t LT_TxBuffSz = 0;

static uint8 *LT_RxBuffer = NULL;
static size_t LT_RxBuffSz = 0;
static int rx_data_offset = 0;

static uint8 *MyRxBuffer = NULL;
static struct sockaddr_in MyRxAddress;
static my_SOCKET sock_fd = my_INVALID_SOCKET;
static bool udp_ok = false;

#ifdef _WIN32
static bool have_winsock = false;
#endif

static uint32 LT_MyStamp = 0;
static uint8 LT_NodeHint = 0;

static bool CTSpacketPending = false;
static uint8 CTSpacketRxDA = 0;
static uint8 CTSpacketRxSA = 0;
static uint8 MyCTSBuffer[4];

static uint8 my_node_address = 0;
static bool LTAddrSrchMd = false;

static void start_udp(void)
{
#ifdef _WIN32
	WSADATA wsaData;
	if (0 != WSAStartup(MAKEWORD(2, 2), &wsaData)) {
		SCC_LOG("start_udp: WSAStartup failed\n");
		return;
	}
	have_winsock = true;
#endif

	if (my_INVALID_SOCKET == (sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))) {
		SCC_LOG("start_udp: socket creation failed\n");
		return;
	}

	int one = 1;
	setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));

#ifdef SO_REUSEPORT
	setsockopt(sock_fd, SOL_SOCKET, SO_REUSEPORT, (const char *)&one, sizeof(one));
#endif

	setsockopt(sock_fd, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof(one));

	unsigned char loop = 1;
	setsockopt(sock_fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *)&loop, sizeof(loop));

	unsigned char ttl = 1;
	setsockopt(sock_fd, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(1954);

	if (0 != bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr))) {
		SCC_LOG("start_udp: bind to port 1954 failed: %s\n", strerror(errno));
		my_closesocket(sock_fd);
		sock_fd = my_INVALID_SOCKET;
		return;
	}

	struct ip_mreq mreq;
	mreq.imr_multiaddr.s_addr = inet_addr("239.192.76.84");
	mreq.imr_interface.s_addr = htonl(INADDR_ANY);

	if (0 != setsockopt(sock_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mreq, sizeof(mreq))) {
		SCC_LOG("start_udp: IP_ADD_MEMBERSHIP for 239.192.76.84 failed: %s\n", strerror(errno));
	}

#ifdef _WIN32
	u_long iMode = 1;
	ioctlsocket(sock_fd, FIONBIO, &iMode);
#else
	int flags = fcntl(sock_fd, F_GETFL, 0);
	fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);
#endif

	udp_ok = true;
	SCC_LOG("start_udp: socket ready on 239.192.76.84:1954 (fd=%d)\n", (int)sock_fd);
}

static void LT_PickStampNodeHint(void)
{
	LT_MyStamp = ((uint32)rand() << 16) ^ (uint32)rand() ^ (uint32)time(NULL);
	LT_NodeHint = (rand() & 0x7E) + 1; // 1..127
	SCC_LOG("LT_PickStampNodeHint: stamp=%08x, nodeHint=%d\n", LT_MyStamp, LT_NodeHint);
}

static void embedMyPID(void)
{
#ifdef _WIN32
	uint32 v = (uint32)GetCurrentProcessId();
#else
	uint32 v = (uint32)getpid();
#endif
	for (int i = 0; i < 4; i++) {
		tx_buffer[i] = (v >> ((3 - i) * 8)) & 0xff;
	}
}

static int pidInPacketIsMine(void)
{
#ifdef _WIN32
	uint32 v = (uint32)GetCurrentProcessId();
#else
	uint32 v = (uint32)getpid();
#endif
	for (int i = 0; i < 4; i++) {
		if (MyRxBuffer[i] != ((v >> ((3 - i) * 8)) & 0xff)) {
			return 0;
		}
	}
	return 1;
}

static int ipInPacketIsMine(void)
{
	if (MyRxAddress.sin_family != AF_INET) {
		return 1; // Drop non-inet
	}
	in_addr_t raddr = MyRxAddress.sin_addr.s_addr;

#ifdef _WIN32
	ULONG bufLen = 15000;
	PIP_ADAPTER_ADDRESSES pAddresses = (IP_ADAPTER_ADDRESSES *)malloc(bufLen);
	if (!pAddresses) return 0;
	if (GetAdaptersAddresses(AF_INET, 0, NULL, pAddresses, &bufLen) == ERROR_BUFFER_OVERFLOW) {
		free(pAddresses);
		pAddresses = (IP_ADAPTER_ADDRESSES *)malloc(bufLen);
		if (!pAddresses) return 0;
	}
	int foundAddress = 0;
	if (GetAdaptersAddresses(AF_INET, 0, NULL, pAddresses, &bufLen) == NO_ERROR) {
		for (PIP_ADAPTER_ADDRESSES pCurr = pAddresses; pCurr; pCurr = pCurr->Next) {
			for (PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurr->FirstUnicastAddress; pUnicast; pUnicast = pUnicast->Next) {
				if (pUnicast->Address.lpSockaddr && pUnicast->Address.lpSockaddr->sa_family == AF_INET) {
					struct sockaddr_in *sa = (struct sockaddr_in *)pUnicast->Address.lpSockaddr;
					if (sa->sin_addr.s_addr == raddr) {
						foundAddress = 1;
						break;
					}
				}
			}
			if (foundAddress) break;
		}
	}
	free(pAddresses);
	return foundAddress;
#else
	struct ifaddrs *iflist, *ifptr;
	int foundAddress = 0;

	if (getifaddrs(&iflist) == 0) {
		for (ifptr = iflist; ifptr; ifptr = ifptr->ifa_next) {
			if (!ifptr->ifa_addr || ifptr->ifa_addr->sa_family != AF_INET) {
				continue;
			}
			struct sockaddr_in *addr = (struct sockaddr_in *)ifptr->ifa_addr;
			if (addr->sin_addr.s_addr == raddr) {
				foundAddress = 1;
				break;
			}
		}
		freeifaddrs(iflist);
	}
	return foundAddress;
#endif
}

static int packetIsOneISent(void)
{
	if (pidInPacketIsMine()) {
		return ipInPacketIsMine();
	}
	return 0;
}

static void LT_TransmitPacket(void)
{
	embedMyPID();
	if (udp_ok && sock_fd != my_INVALID_SOCKET) {
		struct sockaddr_in dest;
		memset(&dest, 0, sizeof(dest));
		dest.sin_family = AF_INET;
		dest.sin_addr.s_addr = inet_addr("239.192.76.84");
		dest.sin_port = htons(1954);

		SCC_LOG("LT TX: len=%lu, da=%d, sa=%d, type=0x%02x\n",
			(unsigned long)LT_TxBuffSz,
			LT_TxBuffSz > 0 ? LT_TxBuffer[0] : 0,
			LT_TxBuffSz > 1 ? LT_TxBuffer[1] : 0,
			LT_TxBuffSz > 2 ? LT_TxBuffer[2] : 0);

		sendto(sock_fd, (const char *)tx_buffer, LT_TxBuffSz + 4, 0,
			(struct sockaddr *)&dest, sizeof(dest));
	}
}

static int GetNextPacket(void)
{
	if (!udp_ok || sock_fd == my_INVALID_SOCKET) return -1;
	socklen_t addrlen = sizeof(MyRxAddress);
	int bytes = recvfrom(sock_fd, (char *)MyRxBuffer,
		RX_BUFFER_ALLOCATION, 0,
		(struct sockaddr *)&MyRxAddress, &addrlen);
	return bytes;
}

static void LT_ReceivePacket(void)
{
	int bytes;
label_retry:
	bytes = GetNextPacket();
	if (bytes > 0) {
		if (packetIsOneISent()) {
			goto label_retry;
		}
		if (bytes >= 4) {
			LT_RxBuffer = MyRxBuffer + 4;
			LT_RxBuffSz = bytes - 4;
			SCC_LOG("LT RX: len=%lu, da=%d, sa=%d, type=0x%02x\n",
				(unsigned long)LT_RxBuffSz,
				LT_RxBuffSz > 0 ? LT_RxBuffer[0] : 0,
				LT_RxBuffSz > 1 ? LT_RxBuffer[1] : 0,
				LT_RxBuffSz > 2 ? LT_RxBuffer[2] : 0);
		}
	}
}

static bool InitLocalTalk(void)
{
	LT_PickStampNodeHint();
	LT_TxBuffer = &tx_buffer[4];
	LT_TxBuffSz = 0;
	LT_RxBuffer = NULL;
	LT_RxBuffSz = 0;

	if (!MyRxBuffer) {
		MyRxBuffer = (uint8 *)malloc(RX_BUFFER_ALLOCATION);
		if (!MyRxBuffer) return false;
	}

	start_udp();
	return udp_ok;
}

static void UnInitLocalTalk(void)
{
	if (my_INVALID_SOCKET != sock_fd) {
		my_closesocket(sock_fd);
		sock_fd = my_INVALID_SOCKET;
	}
#ifdef _WIN32
	if (have_winsock) {
		WSACleanup();
		have_winsock = false;
	}
#endif
	if (MyRxBuffer) {
		free(MyRxBuffer);
		MyRxBuffer = NULL;
	}
	udp_ok = false;
}

/*
 *  SCC register & interrupt logic
 */

static void CheckSCCInterruptFlag(void)
{
	bool ReceiveBInterrupt = false;
	bool RxSpclBInterrupt = SCC.a[1].EndOfFrame;

	switch (SCC.a[1].RxIntMode) {
		case 0: /* disabled */
			RxSpclBInterrupt = false;
			break;
		case 1: /* Rx INT on 1st char or special condition */
			if (SCC.a[1].RxChrAvail && SCC.a[1].FirstChar) {
				ReceiveBInterrupt = true;
			}
			break;
		case 2: /* INT on all Rx char or special condition */
			if (SCC.a[1].RxChrAvail) {
				ReceiveBInterrupt = true;
			}
			break;
		case 3: /* Rx INT on special condition only */
			break;
	}

	bool ExtBInterrupt = SCC.a[1].ExtIE && SCC.a[1].ExtIP;

	int old_type = SCC.SCC_Interrupt_Type;
	if (!SCC.MIE) {
		SCC.SCC_Interrupt_Type = 0;
	} else if (SCC.a[0].TxIP && SCC.a[0].TxIE) {
		SCC.SCC_Interrupt_Type = SCC_A_Tx_Empty;
	} else if (ReceiveBInterrupt) {
		SCC.SCC_Interrupt_Type = SCC_B_Rx;
	} else if (RxSpclBInterrupt) {
		SCC.SCC_Interrupt_Type = SCC_B_Rx_Spec;
	} else if (SCC.a[1].TxIP && SCC.a[1].TxIE) {
		SCC.SCC_Interrupt_Type = SCC_B_Tx_Empty;
	} else if (ExtBInterrupt) {
		SCC.SCC_Interrupt_Type = SCC_B_Ext;
	} else {
		SCC.SCC_Interrupt_Type = 0;
	}

	uint8 NewSCCInterruptRequest = (SCC.SCC_Interrupt_Type != 0) ? 1 : 0;
	if (NewSCCInterruptRequest != SCCInterruptRequest || old_type != SCC.SCC_Interrupt_Type) {
		SCC_LOG("INT: type %d -> %d, req %d -> %d (MIE=%d, RxMode=%d, RxAvail=%d, FirstChar=%d, EOF=%d, TxIP=%d, ExtIP=%d)\n",
			old_type, SCC.SCC_Interrupt_Type, SCCInterruptRequest, NewSCCInterruptRequest,
			SCC.MIE, SCC.a[1].RxIntMode, SCC.a[1].RxChrAvail, SCC.a[1].FirstChar, SCC.a[1].EndOfFrame, SCC.a[1].TxIP, SCC.a[1].ExtIP);
		SCCInterruptRequest = NewSCCInterruptRequest;
		if (SCCInterruptRequest) {
			TriggerInterrupt();
		}
	}
}

static void LT_TransmitPacket1(void);

static void try_extract_tx_packets(void)
{
	if (LT_TxBuffSz < 3) return;

	uint8 ptype = LT_TxBuffer[2];
	size_t expected_len = 0;

	if (ptype >= 0x80) {
		/* Control packet (RTS, CTS, ENQ, ACK) - always exactly 3 bytes */
		expected_len = 3;
	} else {
		/* Data packet (DDP Short or Long) */
		if (LT_TxBuffSz < 5) return;
		/* DDP length is in the first 10 bits of the 2-byte field at offset 3 */
		size_t ddp_len = (((size_t)(LT_TxBuffer[3] & 0x03)) << 8) | (size_t)LT_TxBuffer[4];
		if (ddp_len == 0 || ddp_len > 600) {
			return;
		}
		expected_len = 3 + ddp_len;
	}

	if (LT_TxBuffSz >= expected_len) {
		LT_TransmitPacket1();
	}
}

static void SCC_TxBuffPut(uint32 Data)
{
	if (LT_TxBuffSz < LT_TxBfMxSz) {
		LT_TxBuffer[LT_TxBuffSz] = (uint8)Data;
		++LT_TxBuffSz;
	}
	try_extract_tx_packets();
}

static void GetCTSpacket(void)
{
	MyCTSBuffer[0] = CTSpacketRxDA;
	MyCTSBuffer[1] = CTSpacketRxSA;
	MyCTSBuffer[2] = 0x85; // lapCTS

	LT_RxBuffer = MyCTSBuffer;
	LT_RxBuffSz = 3;
	CTSpacketPending = false;
	SCC_LOG("LT: returning faked CTS packet\n");
}

static void GetNextPacketForMe(void)
{
label_retry:
	LT_ReceivePacket();

	if (NULL != LT_RxBuffer) {
		uint8 dst = LT_RxBuffer[0];
		if (LTAddrSrchMd && my_node_address != 0 && (dst != my_node_address) && (dst != 0xFF)) {
			LT_RxBuffer = NULL;
			goto label_retry;
		}
	}
}

static void LT_ReceivePacket1(void)
{
	if (CTSpacketPending) {
		GetCTSpacket();
	} else {
		GetNextPacketForMe();
	}
}

static void rx_complete(void)
{
	LT_RxBuffer = NULL;
	LT_RxBuffSz = 0;
	rx_data_offset = 0;
	SCC.a[1].EndOfFrame = true;
	SCC.a[1].SyncHunt = true;
	SCC.a[1].RxChrAvail = false;
	SCC.a[1].RxBuff = 0x7E;
	SCC_LOG("LT: rx_complete, EndOfFrame=true, RxChrAvail=false\n");
}

static void SCC_RxBuffAdvance(void)
{
	if (NULL == LT_RxBuffer) {
		SCC.a[1].RxBuff = 0x7E;
		SCC.a[1].RxChrAvail = false;
		return;
	}

	if (rx_data_offset < (int)LT_RxBuffSz) {
		SCC.a[1].RxBuff = LT_RxBuffer[rx_data_offset];
		SCC.a[1].RxChrAvail = true;
	} else {
		int crc_offset = rx_data_offset - (int)LT_RxBuffSz;
		if (crc_offset == 0) {
			/* 1st CRC byte */
			SCC.a[1].RxBuff = 0;
			SCC.a[1].RxChrAvail = true;
		} else if (crc_offset == 1) {
			/* 2nd CRC byte - signal End of Frame */
			SCC.a[1].RxBuff = 0;
			SCC.a[1].RxChrAvail = true;
			SCC.a[1].EndOfFrame = true;
			SCC.a[1].SyncHunt = true;
		} else {
			/* Past CRC bytes - frame complete */
			rx_complete();
			return;
		}
	}
	++rx_data_offset;
}

void LocalTalkTick(void)
{
	if (!ltoudp_active) return;

	if (SCC.a[1].RxEnable && (!SCC.a[1].RxChrAvail)) {
		if (NULL == LT_RxBuffer) {
			LT_ReceivePacket1();
		}
		if (NULL != LT_RxBuffer) {
			rx_data_offset = 0;
			SCC.a[1].EndOfFrame = false;
			SCC.a[1].RxChrAvail = true;
			SCC.a[1].FirstChar = true;

			SCC_RxBuffAdvance();
			CheckSCCInterruptFlag();
		}
	}
}

static void LT_TransmitPacket1(void)
{
	if (LT_TxBuffSz >= 3) {
		uint8 dst = LT_TxBuffer[0];
		uint8 src = LT_TxBuffer[1];
		uint8 type = LT_TxBuffer[2];

		if (src != 0 && src != 0xFF) {
			my_node_address = src;
		}

		if (type < 0x80) {
			/* data packet */
			LT_TransmitPacket();
		} else {
			/* control packet */
			if (0x81 == type) {
				/* lapENQ - Node ID probe */
				LT_TransmitPacket();
			} else if (0x82 == type) {
				/* lapACK - Response to ENQ */
				LT_TransmitPacket();
			} else if (0x84 == type) {
				/* lapRTS - Request to send (both unicast and broadcast get local synthesized CTS) */
				SCC_LOG("LT: lapRTS received, faking CTS (DA=%d, SA=%d)\n", dst, src);
				CTSpacketRxDA = src; /* rx da = tx sa */
				CTSpacketRxSA = dst; /* rx sa = tx da */
				CTSpacketPending = true;

				/* Immediately inject the CTS response into the receiver */
				if (SCC.a[1].RxEnable) {
					GetCTSpacket();
					rx_data_offset = 0;
					SCC.a[1].EndOfFrame = false;
					SCC.a[1].RxChrAvail = true;
					SCC.a[1].SyncHunt = false;
					SCC.a[1].FirstChar = true;
					SCC_RxBuffAdvance();
					CheckSCCInterruptFlag();
				}
			} else if (0x85 == type) {
				/* ignore lapCTS - don't send over network */
			} else {
				LT_TransmitPacket();
			}
		}
	}
	LT_TxBuffSz = 0;
}

static void SCC_InitChannel(int chan)
{
	SCC.a[chan].SyncHunt = true;
	SCC.a[chan].BaudLo = 0;
	SCC.a[chan].BaudHi = 0;
	SCC.a[chan].WR15 = 0;
	SCC.a[chan].ExtIP = false;
	SCC.a[chan].BreakAbort = false;
	SCC.a[chan].SendBreak = false;
}

static void SCC_ResetChannel(int chan)
{
	SCC.a[chan].RxBuff = 0;
	SCC.a[chan].RxChrAvail = false;
	SCC.a[chan].TxBufferEmpty = true;
	SCC.a[chan].TxUnderrun = true;
	SCC.a[chan].EndOfFrame = false;
	SCC.a[chan].ExtIE = false;
	SCC.a[chan].ExtIP = false;
	SCC.a[chan].FirstChar = true;
	SCC.a[chan].BreakAbort = false;
	SCC.a[chan].SendBreak = false;
	if (chan == 1) {
		LT_RxBuffer = NULL;
		LT_RxBuffSz = 0;
		rx_data_offset = 0;
		CTSpacketPending = false;
	}
}

void SCC_Reset(void)
{
	SCC_LOG("SCC_Reset\n");
	SCC.PointerBits = 0;
	SCC.MIE = false;
	SCC.InterruptVector = 0;
	SCCInterruptRequest = 0;

	SCC_InitChannel(1);
	SCC_InitChannel(0);

	SCC_ResetChannel(1);
	SCC_ResetChannel(0);
}

static uint32 SCC_GetRR0(int chan)
{
	if (chan == 1) {
		LocalTalkTick();
	}
	uint32 val = (SCC.a[chan].BreakAbort ? (1 << 7) : 0) /* Break/Abort */
		| (SCC.a[chan].TxUnderrun ? (1 << 6) : 0)
		| (1 << 5) /* CTS asserted */
		| (SCC.a[chan].SyncHunt ? (1 << 4) : 0) /* Sync/Hunt */
		| (1 << 3) /* DCD asserted */
		| (SCC.a[chan].TxBufferEmpty ? (1 << 2) : 0)
		| (SCC.a[chan].RxChrAvail ? (1 << 0) : 0);
	return val;
}

static uint32 SCC_GetRR1(int chan)
{
	uint32 value = Bit0; /* All Sent */
	if (SCC.a[chan].EndOfFrame) {
		value |= (1 << 7); /* End of Frame */
	}
	return value;
}

static uint32 SCC_GetRR2(int chan)
{
	if (chan == 0) {
		return SCC.InterruptVector;
	} else {
		uint32 val = SCC.InterruptVector & 0x70;
		int type = SCC.SCC_Interrupt_Type;
		switch (type) {
			case SCC_A_Rx_Spec:  val |= (7 << 1); break;
			case SCC_A_Rx:       val |= (6 << 1); break;
			case SCC_A_Ext:      val |= (5 << 1); break;
			case SCC_A_Tx_Empty: val |= (4 << 1); break;
			case SCC_B_Rx_Spec:  val |= (3 << 1); SCC.a[1].EndOfFrame = false; break;
			case SCC_B_Rx:       val |= (2 << 1); SCC.a[1].FirstChar = false; break;
			case SCC_B_Ext:      val |= (1 << 1); SCC.a[1].ExtIP = false; break;
			case SCC_B_Tx_Empty: val |= (0 << 1); SCC.a[1].TxIP = false; break;
			default:             val |= (3 << 1); break; // No int pending
		}
		CheckSCCInterruptFlag();
		return val;
	}
}

static uint32 SCC_GetRR3(int chan)
{
	if (chan == 0) {
		uint32 val = 0;
		if (SCC.a[0].RxChrAvail) val |= (1 << 5);
		if (SCC.a[0].TxIP && SCC.a[0].TxIE) val |= (1 << 4);
		if (SCC.a[0].ExtIP && SCC.a[0].ExtIE) val |= (1 << 3);
		if (SCC.a[1].RxChrAvail) val |= (1 << 2);
		if (SCC.a[1].TxIP && SCC.a[1].TxIE) val |= (1 << 1);
		if (SCC.a[1].ExtIP && SCC.a[1].ExtIE) val |= (1 << 0);
		return val;
	}
	return 0;
}

static uint32 SCC_GetRR8(int chan)
{
	uint32 value = 0;
	if (SCC.a[chan].RxEnable) {
		if (0 != chan) {
			value = SCC.a[1].RxBuff;
			SCC.a[1].FirstChar = false;
			SCC_RxBuffAdvance();
		} else {
			value = 0x7E;
		}
	}
	return value;
}

static uint32 SCC_GetRR10(int chan)
{
	return 0;
}

static uint32 SCC_GetRR12(int chan)
{
	return SCC.a[chan].BaudLo;
}

static uint32 SCC_GetRR13(int chan)
{
	return SCC.a[chan].BaudHi;
}

static uint32 SCC_GetRR15(int chan)
{
	return SCC.a[chan].WR15;
}

static uint32 SCC_GetReg(int chan, uint32 SCC_Reg)
{
	uint32 value;
	switch (SCC_Reg) {
		case 0:  value = SCC_GetRR0(chan); break;
		case 1:  value = SCC_GetRR1(chan); break;
		case 2:  value = SCC_GetRR2(chan); break;
		case 3:  value = SCC_GetRR3(chan); break;
		case 8:  value = SCC_GetRR8(chan); break;
		case 10: value = SCC_GetRR10(chan); break;
		case 12: value = SCC_GetRR12(chan); break;
		case 13: value = SCC_GetRR13(chan); break;
		case 15: value = SCC_GetRR15(chan); break;
		default: value = 0; break;
	}
	CheckSCCInterruptFlag();
	return value;
}

static void SCC_PutWR0(uint32 Data, int chan)
{
	switch ((Data >> 6) & 3) {
		case 1:
			SCC_LOG("WR0 [%c]: Reset Rx CRC Checker\n", chan == 0 ? 'A' : 'B');
			break;
		case 2:
			SCC_LOG("WR0 [%c]: Reset Tx CRC Generator\n", chan == 0 ? 'A' : 'B');
			break;
		case 3:
			SCC_LOG("WR0 [%c]: Reset Tx Underrun/EOM Latch\n", chan == 0 ? 'A' : 'B');
			if (0 != chan) {
				LT_TransmitPacket1();
			}
			break;
		default:
			break;
	}
	SCC.PointerBits = Data & 0x07;
	switch ((Data >> 3) & 7) {
		case 1: /* Point High */
			SCC.PointerBits |= 8;
			SCC_LOG("WR0 [%c]: Point High (PointerBits=%d)\n", chan == 0 ? 'A' : 'B', SCC.PointerBits);
			break;
		case 2: /* Reset Ext/Status Ints */
			SCC.a[chan].SyncHunt = false;
			SCC.a[chan].EndOfFrame = false;
			SCC.a[chan].ExtIP = false;
			if (!SCC.a[chan].SendBreak) {
				SCC.a[chan].BreakAbort = false;
			}
			SCC_LOG("WR0 [%c]: Reset Ext/Status Ints\n", chan == 0 ? 'A' : 'B');
			CheckSCCInterruptFlag();
			break;
		case 3: /* Send Abort (SDLC) */
			SCC.a[chan].BreakAbort = true;
			SCC.a[chan].TxBufferEmpty = true;
			SCC_LOG("WR0 [%c]: Send Abort (SDLC)\n", chan == 0 ? 'A' : 'B');
			break;
		case 4: /* Enable Int on next Rx char */
			SCC_LOG("WR0 [%c]: Enable Int on next Rx char\n", chan == 0 ? 'A' : 'B');
			SCC.a[chan].FirstChar = true;
			SCC.a[chan].SyncHunt = true;
			if (0 != chan && NULL != LT_RxBuffer) {
				/* Mac driver discarded the rest of the current packet */
				LT_RxBuffer = NULL;
				LT_RxBuffSz = 0;
				rx_data_offset = 0;
				SCC.a[1].RxChrAvail = false;
				SCC.a[1].RxBuff = 0x7E;
				SCC.a[1].EndOfFrame = false;
			}
			LocalTalkTick();
			break;
		case 5: /* Reset Tx Int Pending */
			SCC.a[chan].TxIP = false;
			SCC_LOG("WR0 [%c]: Reset Tx Int Pending\n", chan == 0 ? 'A' : 'B');
			CheckSCCInterruptFlag();
			break;
		case 6: /* Error Reset */
			SCC.a[chan].EndOfFrame = false;
			SCC_LOG("WR0 [%c]: Error Reset\n", chan == 0 ? 'A' : 'B');
			CheckSCCInterruptFlag();
			break;
		default:
			break;
	}
}

static void SCC_PutWR1(uint32 Data, int chan)
{
	SCC.a[chan].ExtIE = (Data & Bit0) != 0;
	SCC.a[chan].TxIE = (Data & Bit1) != 0;
	SCC.a[chan].RxIntMode = (Data >> 3) & 3;
	if (SCC.a[chan].RxIntMode == 1) {
		SCC.a[chan].FirstChar = true;
	}
	SCC_LOG("WR1 [%c]: ExtIE=%d, TxIE=%d, RxIntMode=%d\n", chan == 0 ? 'A' : 'B',
		SCC.a[chan].ExtIE, SCC.a[chan].TxIE, SCC.a[chan].RxIntMode);
}

static void SCC_PutWR2(uint32 Data, int chan)
{
	SCC.InterruptVector = (uint8)Data;
	SCC_LOG("WR2 [%c]: InterruptVector=0x%02x\n", chan == 0 ? 'A' : 'B', SCC.InterruptVector);
}

static void SCC_PutWR3(uint32 Data, int chan)
{
	bool NewRxEnable = (Data & Bit0) != 0;
	SCC.a[chan].RxEnable = NewRxEnable;
	SCC.a[chan].AddrSrchMd = (Data & Bit2) != 0;
	if (Data & Bit4) {
		SCC.a[chan].SyncHunt = true;
		if (chan == 1 && NULL == LT_RxBuffer) {
			SCC.a[1].EndOfFrame = false;
		}
	}
	if (0 != chan) {
		LTAddrSrchMd = SCC.a[chan].AddrSrchMd;
		if (!NewRxEnable) {
			/* Go back to idle state */
			SCC.a[chan].EndOfFrame = false;
			SCC.a[chan].RxChrAvail = false;
			SCC.a[chan].SyncHunt = true;
			LT_RxBuffer = NULL;
			LT_RxBuffSz = 0;
			rx_data_offset = 0;
		} else {
			LocalTalkTick();
		}
	}
	SCC_LOG("WR3 [%c]: RxEnable=%d, AddrSrchMd=%d, Hunt=%d\n", chan == 0 ? 'A' : 'B',
		SCC.a[chan].RxEnable, SCC.a[chan].AddrSrchMd, (Data & Bit4) != 0);
}

static void SCC_PutWR4(uint32 Data, int chan)
{
	SCC.a[chan].SyncMode = (Data >> 4) & 3;
	SCC_LOG("WR4 [%c]: SyncMode=%d\n", chan == 0 ? 'A' : 'B', SCC.a[chan].SyncMode);
}

static void SCC_PutWR5(uint32 Data, int chan)
{
	SCC.a[chan].TxEnable = (Data & Bit3) != 0;
	SCC.a[chan].SendBreak = (Data & Bit4) != 0;
	SCC.a[chan].BreakAbort = SCC.a[chan].SendBreak;
	if (!SCC.a[chan].TxEnable && 0 != chan && LT_TxBuffSz >= 3) {
		LT_TransmitPacket1();
	}
	SCC_LOG("WR5 [%c]: TxEnable=%d, SendBreak=%d, BreakAbort=%d\n", chan == 0 ? 'A' : 'B',
		SCC.a[chan].TxEnable, SCC.a[chan].SendBreak, SCC.a[chan].BreakAbort);
}

static void SCC_PutWR6(uint32 Data, int chan)
{
	if (2 == SCC.a[chan].SyncMode) {
		if (0 != chan) {
			my_node_address = (uint8)Data;
			SCC_LOG("WR6 [%c]: Set my_node_address=%d\n", chan == 0 ? 'A' : 'B', my_node_address);
		}
	}
}

static void SCC_PutWR7(uint32 Data, int chan)
{
	SCC_LOG("WR7 [%c]: SDLC Flag 0x%02x\n", chan == 0 ? 'A' : 'B', Data & 0xff);
}

static void SCC_PutWR8(uint32 Data, int chan)
{
	if (0 != chan) {
		SCC_TxBuffPut(Data);
	}
	SCC.a[chan].TxIP = true;
	SCC.a[chan].TxBufferEmpty = true;
	CheckSCCInterruptFlag();
}

static void SCC_PutWR9(uint32 Data, int chan)
{
	switch ((Data >> 6) & 3) {
		case 1: SCC_LOG("WR9: Reset Channel B\n"); SCC_ResetChannel(1); break;
		case 2: SCC_LOG("WR9: Reset Channel A\n"); SCC_ResetChannel(0); break;
		case 3: SCC_LOG("WR9: Hardware Reset\n"); SCC_Reset(); break;
		default: break;
	}
	SCC.MIE = (Data & Bit3) != 0;
	SCC_LOG("WR9: MIE=%d\n", SCC.MIE);
	CheckSCCInterruptFlag();
}

static void SCC_PutWR10(uint32 Data, int chan)
{
	SCC_LOG("WR10 [%c]: 0x%02x\n", chan == 0 ? 'A' : 'B', Data & 0xff);
}

static void SCC_PutWR11(uint32 Data, int chan)
{
	SCC_LOG("WR11 [%c]: 0x%02x\n", chan == 0 ? 'A' : 'B', Data & 0xff);
}

static void SCC_PutWR12(uint32 Data, int chan)
{
	SCC.a[chan].BaudLo = Data;
	SCC_LOG("WR12 [%c]: BaudLo=0x%02x\n", chan == 0 ? 'A' : 'B', Data & 0xff);
}

static void SCC_PutWR13(uint32 Data, int chan)
{
	SCC.a[chan].BaudHi = Data;
	SCC_LOG("WR13 [%c]: BaudHi=0x%02x\n", chan == 0 ? 'A' : 'B', Data & 0xff);
}

static void SCC_PutWR14(uint32 Data, int chan)
{
	SCC_LOG("WR14 [%c]: 0x%02x\n", chan == 0 ? 'A' : 'B', Data & 0xff);
}

static void SCC_PutWR15(uint32 Data, int chan)
{
	SCC.a[chan].WR15 = Data;
	SCC_LOG("WR15 [%c]: 0x%02x\n", chan == 0 ? 'A' : 'B', Data & 0xff);
}

static void SCC_PutReg(uint32 Data, int chan, uint32 SCC_Reg)
{
	switch (SCC_Reg) {
		case 0:  SCC_PutWR0(Data, chan); break;
		case 1:  SCC_PutWR1(Data, chan); break;
		case 2:  SCC_PutWR2(Data, chan); break;
		case 3:  SCC_PutWR3(Data, chan); break;
		case 4:  SCC_PutWR4(Data, chan); break;
		case 5:  SCC_PutWR5(Data, chan); break;
		case 6:  SCC_PutWR6(Data, chan); break;
		case 7:  SCC_PutWR7(Data, chan); break;
		case 8:  SCC_PutWR8(Data, chan); break;
		case 9:  SCC_PutWR9(Data, chan); break;
		case 10: SCC_PutWR10(Data, chan); break;
		case 11: SCC_PutWR11(Data, chan); break;
		case 12: SCC_PutWR12(Data, chan); break;
		case 13: SCC_PutWR13(Data, chan); break;
		case 14: SCC_PutWR14(Data, chan); break;
		case 15: SCC_PutWR15(Data, chan); break;
		default: break;
	}
	CheckSCCInterruptFlag();
}

uint32 SCC_Access(uint32 Data, bool WriteMem, uint32 addr)
{
	uint32 SCC_Reg;
	int chan = (~addr) & 1; /* 0=modem (A), 1=printer (B) */
	bool is_data = ((addr >> 1) & 1) != 0;
	if (!is_data) {
		/* Channel Control */
		SCC_Reg = SCC.PointerBits;
		SCC.PointerBits = 0;
	} else {
		/* Channel Data */
		SCC_Reg = 8;
	}

#if SCC_DEBUG
	uint32 pc = m68k_getpc();
	if (WriteMem) {
		SCC_LOG("WR [PC=%08x]: chan=%c (%s) WR%d <- 0x%02x\n",
			pc, chan == 0 ? 'A' : 'B', is_data ? "Data" : "Ctl", SCC_Reg, Data & 0xff);
		SCC_PutReg(Data, chan, SCC_Reg);
	} else {
		Data = SCC_GetReg(chan, SCC_Reg);
		if (is_data) {
			SCC_LOG("RD [PC=%08x]: chan=%c (Data) RR8 -> 0x%02x (offset=%d/%lu)\n",
				pc, chan == 0 ? 'A' : 'B', Data & 0xff, rx_data_offset - 1, (unsigned long)LT_RxBuffSz);
		} else {
			SCC_LOG("RD [PC=%08x]: chan=%c (%s) RR%d -> 0x%02x\n",
				pc, chan == 0 ? 'A' : 'B', "Ctl", SCC_Reg, Data & 0xff);
		}
	}
#else
	if (WriteMem) {
		SCC_PutReg(Data, chan, SCC_Reg);
	} else {
		Data = SCC_GetReg(chan, SCC_Reg);
	}
#endif
	return Data;
}

bool SCC_InterruptsEnabled(void)
{
	return SCC.MIE;
}

void SCCInit(void)
{
	if (PrefsFindBool("ltoudp")) {
		ltoudp_active = InitLocalTalk();
		SCC_Reset();
		scc_initialized = true;
		if (ltoudp_active) {
			printf("SCC: LocalTalk over UDP (LToUDP) enabled on Printer port (239.192.76.84:1954)\n");
		}
	}
}

void SCCExit(void)
{
	if (scc_initialized) {
		if (ltoudp_active) {
			UnInitLocalTalk();
			ltoudp_active = false;
		}
		scc_initialized = false;
	}
}
