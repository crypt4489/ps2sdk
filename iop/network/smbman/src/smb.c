/*
  Copyright 2009-2010, jimmikaelkael
  Licenced under Academic Free License version 3.0
*/

#include <types.h>
#include <errno.h>
#include <irx.h>
#include <stdio.h>
#include <sysclib.h>
#include <ps2ip.h>
#include <ioman.h>

#include "smb.h"
#include "auth.h"
#include "poll.h"
#include "debug.h"

static server_specs_t server_specs;

ReadAndXRequest_t smb_Read_Request = {
	{	SMB_MAGIC,
		SMB_COM_READ_ANDX,
		0, 0, 0, SMB_FLAGS2_32BIT_STATUS, "\0", 0, 0, 0, 0
	},
	12,
	SMB_COM_NONE,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

WriteAndXRequest_t smb_Write_Request = {
	{	SMB_MAGIC,
		SMB_COM_WRITE_ANDX,
		0, 0, 0, SMB_FLAGS2_32BIT_STATUS, "\0", 0, 0, 0, 0
	},
	14,
	SMB_COM_NONE,
	0, 0, 0, 0, 0, 0x01, 0, 0, 0, 0x3f, 0 	// 0x01 is WriteThrough mode and 0x3f is DataOffset
};

#define LM_AUTH 	0
#define NTLM_AUTH 	1

static int main_socket = -1;

static union {
	u8 u8buff[MAX_SMB_BUF+1024];
	u16 u16buff[(MAX_SMB_BUF+1024) / sizeof(u16)];
	s16 s16buff[(MAX_SMB_BUF+1024) / sizeof(s16)];
	struct {
		//Direct transport packet header. This is also a NetBIOS session header.
		u32 sessionHeader; //The lower 24 bytes are the length of the payload in network byte-order, while the upper 8 bits must be set to 0 (Session Message Packet).
		union {
			NegotiateProtocolRequest_t negotiateProtocolRequest;
			NegotiateProtocolResponse_t negotiateProtocolResponse;
			SessionSetupAndXRequest_t sessionSetupAndXRequest;
			SessionSetupAndXResponse_t sessionSetupAndXResponse;
			TreeConnectAndXRequest_t treeConnectAndXRequest;
			TreeConnectAndXResponse_t treeConnectAndXResponse;
			TreeDisconnectRequest_t treeDisconnectRequest;
			TreeDisconnectResponse_t treeDisconnectResponse;
			NetShareEnumRequest_t netShareEnumRequest;
			NetShareEnumResponse_t netShareEnumResponse;
			LogOffAndXRequest_t logOffAndXRequest;
			LogOffAndXResponse_t logOffAndXResponse;
			EchoRequest_t echoRequest;
			EchoResponse_t echoResponse;
			QueryInformationDiskRequest_t queryInformationDiskRequest;
			QueryInformationDiskResponse_t queryInformationDiskResponse;
			QueryPathInformationRequest_t queryPathInformationRequest;
			QueryPathInformationResponse_t queryPathInformationResponse;
			FindFirstNext2Request_t findFirstNext2Request;
			FindFirstNext2Response_t findFirstNext2Response;
			NTCreateAndXRequest_t ntCreateAndXRequest;
			NTCreateAndXResponse_t ntCreateAndXResponse;
			OpenAndXRequest_t openAndXRequest;
			OpenAndXResponse_t openAndXResponse;
			ReadAndXRequest_t readAndXRequest;
			ReadAndXResponse_t readAndXResponse;
			WriteAndXRequest_t writeAndXRequest;
			WriteAndXResponse_t writeAndXResponse;
			CloseRequest_t closeRequest;
			CloseResponse_t closeResponse;
			DeleteRequest_t deleteRequest;
			DeleteResponse_t deleteResponse;
			ManageDirectoryRequest_t manageDirectoryRequest;
			ManageDirectoryResponse_t manageDirectoryResponse;
			RenameRequest_t renameRequest;
			RenameResponse_t renameResponse;
		};
	} __attribute__((packed)) pkt;
} SMB_buf;

//-------------------------------------------------------------------------
server_specs_t *getServerSpecs(void)
{
	return &server_specs;
}

//-------------------------------------------------------------------------
static void nb_SetSessionMessage(u32 size) // Write Session Service header: careful it's raw TCP transport here and not NBT transport
{
	// maximum for raw TCP transport (24 bits) !!!
	// Byte-swap length into network byte-order.
	SMB_buf.pkt.sessionHeader = ((size & 0xff0000) >> 8) | ((size & 0xff00) << 8) | ((size & 0xff) << 24);
}

//-------------------------------------------------------------------------
static int nb_GetSessionMessageLength(void) // Read Session Service header length: careful it's raw TCP transport here and not NBT transport
{
	u32 size;

	// maximum for raw TCP transport (24 bits) !!!
	// Byte-swap length from network byte-order.
	size = ((SMB_buf.pkt.sessionHeader << 8) & 0xff0000) | ((SMB_buf.pkt.sessionHeader >> 8) & 0xff00) | ((SMB_buf.pkt.sessionHeader >> 24) & 0xff);

	return (int)size;
}

static u8 nb_GetPacketType(void) // Read Session Service header type.
{
	// Byte-swap length from network byte-order.
	return((u8)(SMB_buf.pkt.sessionHeader & 0xff));
}

//-------------------------------------------------------------------------
static int OpenTCPSession(struct in_addr dst_IP, u16 dst_port, int *sock)
{
	int sck, ret;
	struct sockaddr_in sock_addr;

	*sock = -1;

	// Creating socket
	sck = lwip_socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sck < 0)
		return -1;

	*sock = sck;

    	memset(&sock_addr, 0, sizeof(sock_addr));
	sock_addr.sin_addr = dst_IP;
	sock_addr.sin_family = AF_INET;
	sock_addr.sin_port = htons(dst_port);

	ret = lwip_connect(sck, (struct sockaddr *)&sock_addr, sizeof(sock_addr));
	if (ret < 0)
		return -2;

	return 0;
}

//-------------------------------------------------------------------------
/*	RecvTimeout() is used instead of just recv(), in order to prevent
	lockups in the event of a network failure in-between calls to socket functions.

	TCP only protects against failures that occur during calls to socket functions.

	Just to illustrate its importance here, imagine what would happen if recv() is called,
	right before the server terminates the connection without the PlayStation 2 ever
	receiving the TCP retransmission and the TCP RST packets.	*/
static int RecvTimeout(int sock, void *buf, int bsize, int timeout_ms)
{
	int ret;
	struct pollfd pollfd[1];

	pollfd->fd = sock;
	pollfd->events = POLLIN;
	pollfd->revents = 0;

	ret = poll(pollfd, 1, timeout_ms);

	// a result less than 0 is an error
	if (ret < 0)
		return -1;

	// 0 is a timeout
	if (ret == 0)
		return 0;

	// receive the packet
	ret = lwip_recv(sock, buf, bsize, 0);
	if (ret < 0)
		return -2;

	return ret;
}

static int SendData(int sock, char *buf, int size)
{
	int remaining, result;
	char *ptr;

	ptr = buf;
	remaining = size;
	while (remaining > 0)
	{
		result = lwip_send(sock, ptr, remaining, 0);
		if (result <= 0)
			return result;

		ptr += result;
		remaining -= result;
	}

	return size;
}

static int RecvData(int sock, char *buf, int size, int timeout_ms)
{
	int remaining, result;
	char *ptr;

	ptr = buf;
	remaining = size;
	while (remaining > 0)
	{
		result = RecvTimeout(sock, ptr, remaining, timeout_ms);
		if (result <= 0)
			return result;

		ptr += result;
		remaining -= result;
	}

	return size;
}

//-------------------------------------------------------------------------
static int GetSMBServerReply(void)
{
	int rcv_size, totalpkt_size;

	//Send the whole message, including the 4-byte direct transport packet header.
	rcv_size = SendData(main_socket, (char *)&SMB_buf.pkt, nb_GetSessionMessageLength() + 4);
	if (rcv_size <= 0)
		return -1;

	//Read NetBIOS session message header. Drop NBSS Session Keep alive messages (type == 0x85, with no body), but process session messages (type == 0x00).
	do{
		rcv_size = RecvData(main_socket, (char *)&SMB_buf.pkt.sessionHeader, sizeof(SMB_buf.pkt.sessionHeader), 10000); // 10s before the packet is considered lost
		if (rcv_size <= 0)
			return -2;
	} while (nb_GetPacketType() != 0);

	// Handle fragmented packets
	totalpkt_size = nb_GetSessionMessageLength();

	rcv_size = RecvData(main_socket, (char *)&SMB_buf.pkt, totalpkt_size, 3000); // 3s before the packet is considered lost
	if (rcv_size <= 0)
		return -2;

	return totalpkt_size;
}

//-------------------------------------------------------------------------
//These functions will process UTF-16 characters on a byte-level, so that they will be safe for use with byte-alignment.
static int asciiToUtf16(char *out, const char *in)
{
	int len;
	const char *pIn;
	u8 *pOut;

	for(pIn = in, pOut = out, len = 0; *pIn != '\0'; pIn++, pOut += 2, len += 2)
	{
		pOut[0] = *pIn;
		pOut[1] = '\0';
	}

	pOut[0] = '\0'; //NULL terminate.
	pOut[1] = '\0';
	len += 2;

	return len;
}

static int utf16ToAscii(char *out, const char *in)
{
	int len;
	const u8 *pIn;
	char *pOut;
	u16 wchar;

	for(pIn = in, pOut = out, len = 0; ; len++)
	{
		wchar = pIn[0] | ((u16)pIn[1] << 8);
		if (wchar == '\0')
			break;

		if (wchar >= 0xD800 && wchar < 0xDC00)
		{	//Skip surrogate. Replace unsupported character with '?'.
			*pOut = '?';

			pIn += 4;
			pOut++;
		} else {
			// Write decoded character. Replace unsupported characters with '?'.
			*pOut = (wchar > 128) ? '?' : (char)wchar;

			pIn += 2;
			pOut++;
		}
	}

	pOut[0] = '\0'; //NULL terminate.
	pOut[1] = '\0';
	len++;

	return len;
}

static int setStringField(char *out, const char *in)
{
	int len;

	if (server_specs.StringsCF == 2)
	{
		len = asciiToUtf16(out, in);
	}
	else
	{
		len = strlen(in) + 1;
		strcpy(out, in);
	}

	return len;
}

static int getStringField(char *out, const char *in)
{
	int len;

	if (server_specs.StringsCF == 2)
	{
		len = utf16ToAscii(out, in);
	}
	else
	{
		len = strlen(in) + 1;
		strcpy(out, in);
	}

	return len;
}

//-------------------------------------------------------------------------
int smb_NegotiateProtocol(u32 *capabilities)
{
	static char *dialect = "NT LM 0.12";
	int r, length, retry_count;
	NegotiateProtocolRequest_t *NPR = &SMB_buf.pkt.negotiateProtocolRequest;
	NegotiateProtocolResponse_t *NPRsp = &SMB_buf.pkt.negotiateProtocolResponse;

	retry_count = 0;

negotiate_retry:

	memset(NPR, 0, sizeof(NegotiateProtocolRequest_t));

	NPR->smbH.Magic = SMB_MAGIC;
	NPR->smbH.Cmd = SMB_COM_NEGOTIATE;
	NPR->smbH.Flags = SMB_FLAGS_CASELESS_PATHNAMES;
	NPR->smbH.Flags2 = SMB_FLAGS2_KNOWS_LONG_NAMES | SMB_FLAGS2_32BIT_STATUS;
	length = strlen(dialect);
	NPR->ByteCount = length + sizeof(NPR->DialectFormat) + 1;
	NPR->DialectFormat = 0x02;
	strcpy(NPR->DialectName, dialect);

	nb_SetSessionMessage(sizeof(NegotiateProtocolRequest_t) + length + 1);
	r = GetSMBServerReply();
	if (r <= 0)
		goto negotiate_error;

	// check sanity of SMB header
	if (NPRsp->smbH.Magic != SMB_MAGIC)
		goto negotiate_error;

	// check there's no error
	if (NPRsp->smbH.Eclass != STATUS_SUCCESS)
		goto negotiate_error;

	if (NPRsp->smbWordcount != 17)
		goto negotiate_error;

	*capabilities = NPRsp->Capabilities & (CLIENT_CAP_LARGE_READX | CLIENT_CAP_UNICODE | CLIENT_CAP_LARGE_FILES | CLIENT_CAP_STATUS32);

	server_specs.StringsCF = (NPRsp->Capabilities & SERVER_CAP_UNICODE) ? 2 : 1;
	server_specs.SupportsNTSMB = (NPRsp->Capabilities & SERVER_CAP_NT_SMBS) ? 1 : 0;
	server_specs.SecurityMode = (NPRsp->SecurityMode & NEGOTIATE_SECURITY_USER_LEVEL) ? SERVER_USER_SECURITY_LEVEL : SERVER_SHARE_SECURITY_LEVEL;
	server_specs.PasswordType = (NPRsp->SecurityMode & NEGOTIATE_SECURITY_CHALLENGE_RESPONSE) ? SERVER_USE_ENCRYPTED_PASSWORD : SERVER_USE_PLAINTEXT_PASSWORD;

	// copy to global to keep needed information for further communication
	server_specs.MaxBufferSize = NPRsp->MaxBufferSize;
	server_specs.MaxMpxCount = NPRsp->MaxMpxCount;
	server_specs.SessionKey = NPRsp->SessionKey;
	memcpy(server_specs.EncryptionKey, NPRsp->ByteField, NPRsp->KeyLength);
	getStringField(server_specs.PrimaryDomainServerName, &NPRsp->ByteField[NPRsp->KeyLength]);

	return 0;

negotiate_error:
	retry_count++;

	if (retry_count < 3)
		goto negotiate_retry;

	return -1;
}

//-------------------------------------------------------------------------
static int AddPassword(char *Password, int PasswordType, int AuthType, u16 *AnsiPassLen, u16 *UnicodePassLen, u8 *Buffer)
{
	u8 passwordhash[16];
	u8 LMresponse[24];
	u16 passwordlen = 0;

	if ((Password) && (PasswordType != NO_PASSWORD)) {
		if (server_specs.PasswordType == SERVER_USE_ENCRYPTED_PASSWORD) {
			passwordlen = 24;
			switch (PasswordType) {
				case HASHED_PASSWORD:
					if (AuthType == LM_AUTH) {
						memcpy(passwordhash, Password, 16);
						memcpy(AnsiPassLen, &passwordlen, 2);
					}
					if (AuthType == NTLM_AUTH) {
						memcpy(passwordhash, &Password[16], 16);
						memcpy(UnicodePassLen, &passwordlen, 2);
					}
					break;

				default:
					if (AuthType == LM_AUTH) {
						LM_Password_Hash((const unsigned char*)Password, passwordhash);
						memcpy(AnsiPassLen, &passwordlen, 2);
					}
					else if (AuthType == NTLM_AUTH) {
						NTLM_Password_Hash((const unsigned char*)Password, passwordhash);
						memcpy(UnicodePassLen, &passwordlen, 2);
					}
			}
			LM_Response(passwordhash, server_specs.EncryptionKey, LMresponse);
			memcpy(Buffer, LMresponse, passwordlen);
		}
		else if (server_specs.PasswordType == SERVER_USE_PLAINTEXT_PASSWORD) {
			// It seems that PlainText passwords and Unicode isn't meant to be...
			passwordlen = strlen(Password);
			if (passwordlen > 14)
				passwordlen = 14;
			else if (passwordlen == 0)
				passwordlen = 1;
			memcpy(AnsiPassLen, &passwordlen, 2);
			memcpy(Buffer, Password, passwordlen);
		}
	}
	else {
		if (server_specs.SecurityMode == SERVER_SHARE_SECURITY_LEVEL) {
			passwordlen = 1;
			memcpy(AnsiPassLen, &passwordlen, 2);
			Buffer[0] = 0;
		}
	}

	return passwordlen;
}

//-------------------------------------------------------------------------
int smb_SessionSetupAndX(char *User, char *Password, int PasswordType, u32 capabilities)
{
	SessionSetupAndXRequest_t *SSR = &SMB_buf.pkt.sessionSetupAndXRequest;
	SessionSetupAndXResponse_t *SSRsp = &SMB_buf.pkt.sessionSetupAndXResponse;
	int r, offset, CF;
	int passwordlen = 0;
	int AuthType = NTLM_AUTH;

lbl_session_setup:

	memset(SSR, 0, sizeof(SessionSetupAndXRequest_t));

	CF = server_specs.StringsCF;

	SSR->smbH.Magic = SMB_MAGIC;
	SSR->smbH.Cmd = SMB_COM_SESSION_SETUP_ANDX;
	SSR->smbH.Flags = SMB_FLAGS_CASELESS_PATHNAMES;
	SSR->smbH.Flags2 = SMB_FLAGS2_KNOWS_LONG_NAMES | SMB_FLAGS2_32BIT_STATUS;
	if (CF == 2)
		SSR->smbH.Flags2 |= SMB_FLAGS2_UNICODE_STRING;
	SSR->smbWordcount = 13;
	SSR->smbAndxCmd = SMB_COM_NONE;		// no ANDX command
	SSR->MaxBufferSize = server_specs.MaxBufferSize > 65535 ? 65535 : (u16)server_specs.MaxBufferSize;
	SSR->MaxMpxCount = server_specs.MaxMpxCount >= 2 ? 2 : (u16)server_specs.MaxMpxCount;
	SSR->VCNumber = 1;
	SSR->SessionKey = server_specs.SessionKey;
	SSR->Capabilities = capabilities;

	// Fill ByteField
	offset = 0;

	if (server_specs.SecurityMode == SERVER_USER_SECURITY_LEVEL) {
		passwordlen = AddPassword(Password, PasswordType, AuthType, &SSR->AnsiPasswordLength, &SSR->UnicodePasswordLength, &SSR->ByteField[0]);
		offset += passwordlen;
	}

	if ((CF == 2) && (!(passwordlen & 1)))
	{
		SSR->ByteField[offset] = '\0';
		offset++;				// pad needed only for unicode as aligment fix if password length is even
	}

	// Add User name
	offset += setStringField(&SSR->ByteField[offset], User);

	// PrimaryDomain, acquired from Negotiate Protocol Response data
	offset += setStringField(&SSR->ByteField[offset], server_specs.PrimaryDomainServerName);

	// NativeOS, NativeLanMan
	memset(&SSR->ByteField[offset], 0, CF * 2);
	offset += CF * 2;

	SSR->ByteCount = offset;

	nb_SetSessionMessage(sizeof(SessionSetupAndXRequest_t)+offset);
	r = GetSMBServerReply();
	if (r <= 0)
		return -EIO;

	// check sanity of SMB header
	if (SSRsp->smbH.Magic != SMB_MAGIC)
		return -EIO;

	// check there's no error (NT STATUS error type!)
	switch(SSRsp->smbH.Eclass | (SSRsp->smbH.Ecode << 16)){
		case STATUS_SUCCESS:
			break;
		case STATUS_LOGON_FAILURE:	// check there's no auth failure
			if ((server_specs.SecurityMode == SERVER_USER_SECURITY_LEVEL)
				&& (AuthType == NTLM_AUTH)) {
				AuthType = LM_AUTH;
				goto lbl_session_setup;
			}

			return -EACCES;
		default:
			return -EIO;
	}

	// return UID
	return (int)SSRsp->smbH.UID;
}

//-------------------------------------------------------------------------
int smb_TreeConnectAndX(int UID, char *ShareName, char *Password, int PasswordType) // PasswordType: 0 = PlainText, 1 = Hash
{
	TreeConnectAndXRequest_t *TCR = &SMB_buf.pkt.treeConnectAndXRequest;
	TreeConnectAndXResponse_t *TCRsp = &SMB_buf.pkt.treeConnectAndXResponse;
	int r, offset, CF;
	int passwordlen = 0;
	int AuthType = NTLM_AUTH;

lbl_tree_connect:

	memset(TCR, 0, sizeof(TreeConnectAndXRequest_t));

	CF = server_specs.StringsCF;

	TCR->smbH.Magic = SMB_MAGIC;
	TCR->smbH.Cmd = SMB_COM_TREE_CONNECT_ANDX;
	TCR->smbH.Flags = SMB_FLAGS_CASELESS_PATHNAMES;
	TCR->smbH.Flags2 = SMB_FLAGS2_KNOWS_LONG_NAMES | SMB_FLAGS2_32BIT_STATUS;
	if (CF == 2)
		TCR->smbH.Flags2 |= SMB_FLAGS2_UNICODE_STRING;
	TCR->smbH.UID = UID;
	TCR->smbWordcount = 4;
	TCR->smbAndxCmd = SMB_COM_NONE;		// no ANDX command

	// Fill ByteField
	offset = 0;

	if (server_specs.SecurityMode == SERVER_SHARE_SECURITY_LEVEL)
		passwordlen = AddPassword(Password, PasswordType, AuthType, &TCR->PasswordLength, &TCR->PasswordLength, &TCR->ByteField[offset]);
	else {
		passwordlen = 1;
		TCR->PasswordLength = passwordlen;
	}
	offset += passwordlen;

	if ((CF == 2) && (!(passwordlen & 1)))
	{
		TCR->ByteField[offset] = '\0';
		offset++;				// pad needed only for unicode as aligment fix is password len is even
	}

	// Add share name
	offset += setStringField(&TCR->ByteField[offset], ShareName);

	memcpy(&TCR->ByteField[offset], "?????\0", 6); 	// Service, any type of device
	offset += 6;

	TCR->ByteCount = offset;

	nb_SetSessionMessage(sizeof(TreeConnectAndXRequest_t)+offset);
	r = GetSMBServerReply();
	if (r <= 0)
		return -EIO;

	// check sanity of SMB header
	if (TCRsp->smbH.Magic != SMB_MAGIC)
		return -EIO;

	// check there's no error (NT STATUS error type!)
	switch(TCRsp->smbH.Eclass | (TCRsp->smbH.Ecode << 16)){
		case STATUS_SUCCESS:
			break;
		case STATUS_LOGON_FAILURE:	// check there's no auth failure
			if ((server_specs.SecurityMode == SERVER_USER_SECURITY_LEVEL)
				&& (AuthType == NTLM_AUTH)) {
				AuthType = LM_AUTH;
				goto lbl_tree_connect;
			}

			return -EACCES;
		default:
			return -EIO;
	}

	// return TID
	return (int)TCRsp->smbH.TID;
}

//-------------------------------------------------------------------------
int smb_NetShareEnum(int UID, int TID, ShareEntry_t *shareEntries, int index, int maxEntries)
{
	int r, i;
	int count = 0;
	NetShareEnumRequest_t *NSER = &SMB_buf.pkt.netShareEnumRequest;
	NetShareEnumResponse_t *NSERsp = &SMB_buf.pkt.netShareEnumResponse;

	memset(NSER, 0, sizeof(NetShareEnumRequest_t));

	NSER->smbH.Magic = SMB_MAGIC;
	NSER->smbH.Cmd = SMB_COM_TRANSACTION;
	NSER->smbH.UID = (u16)UID;
	NSER->smbH.TID = (u16)TID;
	NSER->smbWordcount = 14;

	NSER->smbTrans.TotalParamCount = NSER->smbTrans.ParamCount = 19;
	NSER->smbTrans.MaxParamCount = 1024;
	NSER->smbTrans.MaxDataCount = 8096;
	NSER->smbTrans.ParamOffset = 76;
	NSER->smbTrans.DataOffset = 95;

	NSER->ByteCount = 32;

	// SMB PIPE PROTOCOL
	// Transaction Name: "\PIPE\LANMAN"
	// Function Code : 0x0000 = NetShareEnum
	// Parameter Descriptor: "WrLeh"
	// Return Descriptor: "B13BWz"
	// Detail Level: 0x0001
	// Receive Buffer Length: 0x1fa0
	memcpy(&NSER->ByteField[0], "\\PIPE\\LANMAN\0\0\0WrLeh\0B13BWz\0\x01\0\xa0\x1f", 32);

	nb_SetSessionMessage(sizeof(NetShareEnumRequest_t) + NSER->ByteCount);
	r = GetSMBServerReply();
	if (r <= 0)
		return -EIO;

	// check sanity of SMB header
	if (NSERsp->smbH.Magic != SMB_MAGIC)
		return -EIO;

	switch(NSERsp->smbH.Eclass | (NSERsp->smbH.Ecode << 16)){
		// check there's no error
		case STATUS_SUCCESS:
			break;
		// check if access denied
		case STATUS_ACCESS_DENIED:
			return -EACCES;
		default:
			return -EIO;
	}

	// API status must be 0
	if (SMB_buf.u16buff[(NSERsp->smbTrans.ParamOffset+4) / sizeof(u16)] != 0)
		return -EIO;

	// available entries
	int AvailableEntries = SMB_buf.s16buff[(NSERsp->smbTrans.ParamOffset+4+6) / sizeof(s16)];

	// data start
	char *data = (char *)&SMB_buf.u8buff[NSERsp->smbTrans.DataOffset+4];
	char *p = data;

	for (i=0; i<AvailableEntries; i++) {

		// calculate the padding after the Share name
		int padding = (strlen(p)+1+2) % 16 ? 16-((strlen(p)+1) % 16) : 0;

		if (*((u16 *)&p[strlen(p)+1+padding-2]) == 0) { // Directory Tree type
			if (maxEntries > 0) {
				if ((count < maxEntries) && (i >= index)) {
					count++;
					strncpy(shareEntries->ShareName, p, 256);
					strncpy(shareEntries->ShareComment, &data[*((u16 *)&p[strlen(p)+1+padding])], 256);
					shareEntries++;
				}
			}
			else // if maxEntries is 0 then we're just counting shares
				count++;
		}
		p += strlen(p)+1+padding+4;
	}

	return count;
}

//-------------------------------------------------------------------------
int smb_QueryInformationDisk(int UID, int TID, smbQueryDiskInfo_out_t *QueryInformationDisk)
{
	int r;
	QueryInformationDiskRequest_t *QIDR = &SMB_buf.pkt.queryInformationDiskRequest;
	QueryInformationDiskResponse_t *QIDRsp = &SMB_buf.pkt.queryInformationDiskResponse;

	memset(QIDR, 0, sizeof(QueryInformationDiskRequest_t));

	QIDR->smbH.Magic = SMB_MAGIC;
	QIDR->smbH.Cmd = SMB_COM_QUERY_INFORMATION_DISK;
	QIDR->smbH.UID = (u16)UID;
	QIDR->smbH.TID = (u16)TID;

	nb_SetSessionMessage(sizeof(QueryInformationDiskRequest_t));
	r = GetSMBServerReply();
	if (r <= 0)
		return -EIO;

	// check sanity of SMB header
	if (QIDRsp->smbH.Magic != SMB_MAGIC)
		return -EIO;

	switch(QIDRsp->smbH.Eclass | (QIDRsp->smbH.Ecode << 16)){
		// check there's no error
		case STATUS_SUCCESS:
			break;
		// check if access denied
		case STATUS_ACCESS_DENIED:
			return -EACCES;
		default:
			return -EIO;
	}

	QueryInformationDisk->TotalUnits = QIDRsp->TotalUnits;
	QueryInformationDisk->BlocksPerUnit = QIDRsp->BlocksPerUnit;
	QueryInformationDisk->BlockSize = QIDRsp->BlockSize;
	QueryInformationDisk->FreeUnits = QIDRsp->FreeUnits;

	return 0;
}

//-------------------------------------------------------------------------
int smb_QueryPathInformation(int UID, int TID, PathInformation_t *Info, char *Path)
{
	int r, PathLen, CF, queryType;
	QueryPathInformationRequest_t *QPIR = &SMB_buf.pkt.queryPathInformationRequest;
	QueryPathInformationResponse_t *QPIRsp = &SMB_buf.pkt.queryPathInformationResponse;

	queryType = SMB_QUERY_FILE_BASIC_INFO;

query:

	memset(QPIR, 0, sizeof(QueryPathInformationRequest_t));

	CF = server_specs.StringsCF;

	QPIR->smbH.Magic = SMB_MAGIC;
	QPIR->smbH.Cmd = SMB_COM_TRANSACTION2;
	QPIR->smbH.Flags = SMB_FLAGS_CANONICAL_PATHNAMES;
	QPIR->smbH.Flags2 = SMB_FLAGS2_KNOWS_LONG_NAMES | SMB_FLAGS2_32BIT_STATUS;
	if (CF == 2)
		QPIR->smbH.Flags2 |= SMB_FLAGS2_UNICODE_STRING;
	QPIR->smbH.UID = (u16)UID;
	QPIR->smbH.TID = (u16)TID;
	QPIR->smbWordcount = 15;

	QPIR->smbTrans.SetupCount = 1;
	QPIR->SubCommand = TRANS2_QUERY_PATH_INFORMATION;

	QPIR->smbTrans.ParamOffset = 68;
	QPIR->smbTrans.MaxParamCount = 256; 		// Max Parameters len in reply
	QPIR->smbTrans.MaxDataCount = 16384;		// Max Data len in reply

	QueryPathInformationRequestParam_t *QPIRParam = (QueryPathInformationRequestParam_t *)&SMB_buf.u8buff[QPIR->smbTrans.ParamOffset+4];

	QPIRParam->LevelOfInterest = queryType;
	QPIRParam->Reserved = 0;

	// Add path
	PathLen = setStringField(QPIRParam->FileName, Path);

	QPIR->smbTrans.TotalParamCount = QPIR->smbTrans.ParamCount = 2+4+PathLen;

	QPIR->ByteCount = 3 + QPIR->smbTrans.TotalParamCount;

	QPIR->smbTrans.DataOffset = QPIR->smbTrans.ParamOffset + QPIR->smbTrans.TotalParamCount;

	nb_SetSessionMessage(QPIR->smbTrans.DataOffset);
	r = GetSMBServerReply();
	if (r <= 0)
		return -EIO;

	// check sanity of SMB header
	if (QPIRsp->smbH.Magic != SMB_MAGIC)
		return -EIO;

	// check there's no error
	switch(QPIRsp->smbH.Eclass | (QPIRsp->smbH.Ecode << 16)){
		// check there's no error
		case STATUS_SUCCESS:
			break;
		// check if access denied
		case STATUS_ACCESS_DENIED:
			return -EACCES;
		default:
			return -EIO;
	}

	if (queryType == SMB_QUERY_FILE_BASIC_INFO) {

		BasicFileInfo_t *BFI = (BasicFileInfo_t *)&SMB_buf.u8buff[QPIRsp->smbTrans.DataOffset+4];

		Info->Created = BFI->Created;
		Info->LastAccess = BFI->LastAccess;
		Info->LastWrite = BFI->LastWrite;
		Info->Change = BFI->Change;
		Info->FileAttributes = BFI->FileAttributes;

		// a 2nd query is done with SMB_QUERY_FILE_STANDARD_INFO LevelOfInterest to get a valid 64bit size
		queryType = SMB_QUERY_FILE_STANDARD_INFO;
		goto query;
	}
	else if (queryType == SMB_QUERY_FILE_STANDARD_INFO) {

		StandardFileInfo_t *SFI = (StandardFileInfo_t *)&SMB_buf.u8buff[QPIRsp->smbTrans.DataOffset+4];

		Info->AllocationSize = SFI->AllocationSize;
		Info->EndOfFile = SFI->EndOfFile;
		Info->LinkCount = SFI->LinkCount;
		Info->DeletePending = SFI->DeletePending;
		Info->IsDirectory = SFI->IsDirectory;
	}

	return 0;
}

//-------------------------------------------------------------------------
int smb_NTCreateAndX(int UID, int TID, char *filename, s64 *filesize, int mode)
{
	NTCreateAndXRequest_t *NTCR = &SMB_buf.pkt.ntCreateAndXRequest;
	NTCreateAndXResponse_t *NTCRsp = &SMB_buf.pkt.ntCreateAndXResponse;
	int r, offset, CF;

	memset(NTCR, 0, sizeof(NTCreateAndXRequest_t));

	CF = server_specs.StringsCF;

	NTCR->smbH.Magic = SMB_MAGIC;
	NTCR->smbH.Cmd = SMB_COM_NT_CREATE_ANDX;
	NTCR->smbH.Flags = SMB_FLAGS_CANONICAL_PATHNAMES;
	NTCR->smbH.Flags2 = SMB_FLAGS2_KNOWS_LONG_NAMES | SMB_FLAGS2_32BIT_STATUS;
	if (CF == 2)
		NTCR->smbH.Flags2 |= SMB_FLAGS2_UNICODE_STRING;
	NTCR->smbH.UID = (u16)UID;
	NTCR->smbH.TID = (u16)TID;
	NTCR->smbWordcount = 24;
	NTCR->smbAndxCmd = SMB_COM_NONE;	// no ANDX command
	NTCR->AccessMask = ((mode & O_RDWR) == O_RDWR || (mode & O_WRONLY)) ? 0x2019f : 0x20089;
	NTCR->FileAttributes = ((mode & O_RDWR) == O_RDWR || (mode & O_WRONLY)) ? EXT_ATTR_NORMAL : EXT_ATTR_READONLY;
	NTCR->ShareAccess = 0x01; // Share in read mode only
	if (mode & O_CREAT)
		NTCR->CreateDisposition |= 0x02;
	if (mode & O_TRUNC)
		NTCR->CreateDisposition |= 0x04;
	else
		NTCR->CreateDisposition |= 0x01;
	if (NTCR->CreateDisposition == 0x06)
		NTCR->CreateDisposition = 0x05;
	NTCR->ImpersonationLevel = 2;
	NTCR->SecurityFlags = 0x03;

	offset = 0;
	if (CF == 2)
	{
		NTCR->ByteField[offset] = '\0';
		offset++;				// pad needed only for unicode as aligment fix
	}

	// Add filename
	NTCR->NameLength = setStringField(&NTCR->ByteField[offset], filename);
	offset += NTCR->NameLength;

	NTCR->ByteCount = offset;

	nb_SetSessionMessage(sizeof(NTCreateAndXRequest_t) + offset + 1);
	r = GetSMBServerReply();
	if (r <= 0)
		return -EIO;

	// check sanity of SMB header
	if (NTCRsp->smbH.Magic != SMB_MAGIC)
		return -EIO;

	switch(NTCRsp->smbH.Eclass | (NTCRsp->smbH.Ecode << 16)){
		// check there's no error
		case STATUS_SUCCESS:
			break;
		// check if access denied
		case STATUS_ACCESS_DENIED:
			return -EACCES;
		default:
			return -EIO;
	}

	*filesize = NTCRsp->FileSize;

	return (int)NTCRsp->FID;
}

//-------------------------------------------------------------------------
int smb_OpenAndX(int UID, int TID, char *filename, s64 *filesize, int mode)
{
	// does not supports filesize > 4Gb, so we'll have to use
	// smb_QueryPathInformation to find the real size.
	// OpenAndX is needed for a few NAS units that doesn't supports
	// NT SMB commands set.

	PathInformation_t info;
	OpenAndXRequest_t *OR = &SMB_buf.pkt.openAndXRequest;
	OpenAndXResponse_t *ORsp = &SMB_buf.pkt.openAndXResponse;
	int r, offset, CF;

	if (server_specs.SupportsNTSMB)
		return smb_NTCreateAndX(UID, TID, filename, filesize, mode);

	memset(OR, 0, sizeof(OpenAndXRequest_t));

	CF = server_specs.StringsCF;

	OR->smbH.Magic = SMB_MAGIC;
	OR->smbH.Cmd = SMB_COM_OPEN_ANDX;
	OR->smbH.Flags = SMB_FLAGS_CANONICAL_PATHNAMES;
	OR->smbH.Flags2 = SMB_FLAGS2_KNOWS_LONG_NAMES | SMB_FLAGS2_32BIT_STATUS;
	if (CF == 2)
		OR->smbH.Flags2 |= SMB_FLAGS2_UNICODE_STRING;
	OR->smbH.UID = (u16)UID;
	OR->smbH.TID = (u16)TID;
	OR->smbWordcount = 15;
	OR->smbAndxCmd = SMB_COM_NONE;		// no ANDX command
	OR->AccessMask = ((mode & O_RDWR) == O_RDWR || (mode & O_WRONLY)) ? 0x02 : 0x00;
	OR->FileAttributes = ((mode & O_RDWR) == O_RDWR || (mode & O_WRONLY)) ? EXT_ATTR_NORMAL : EXT_ATTR_READONLY;
	if (mode & O_CREAT)
		OR->CreateOptions |= 0x10;
	if (mode & O_TRUNC)
		OR->CreateOptions |= 0x02;
	else
		OR->CreateOptions |= 0x01;

	offset = 0;
	if (CF == 2)
	{
		OR->ByteField[offset] = '\0';
		offset++;				// pad needed only for unicode as aligment fix
	}

	// Add filename
	offset += setStringField(&OR->ByteField[offset], filename);
	OR->ByteCount = offset;

	nb_SetSessionMessage(sizeof(OpenAndXRequest_t) + offset + 1);
	r = GetSMBServerReply();
	if (r <= 0)
		return -EIO;

	// check sanity of SMB header
	if (ORsp->smbH.Magic != SMB_MAGIC)
		return -EIO;

	switch(ORsp->smbH.Eclass | (ORsp->smbH.Ecode << 16)){
		// check there's no error
		case STATUS_SUCCESS:
			break;
		// check if access denied
		case STATUS_ACCESS_DENIED:
			return -EACCES;
		default:
			return -EIO;
	}

	r = (int)ORsp->FID;

	if(smb_QueryPathInformation(UID, TID, &info, filename) >= 0)
		*filesize = info.EndOfFile;
	else
		r = -1;

	return r;
}

//-------------------------------------------------------------------------
int smb_ReadAndX(int UID, int TID, int FID, s64 fileoffset, void *readbuf, u16 nbytes)
{
	ReadAndXRequest_t *RR = &SMB_buf.pkt.readAndXRequest;
	ReadAndXResponse_t *RRsp = &SMB_buf.pkt.readAndXResponse;
	int r;

	memcpy(RR, &smb_Read_Request, sizeof(ReadAndXRequest_t));

	RR->smbH.UID = (u16)UID;
	RR->smbH.TID = (u16)TID;
	RR->FID = (u16)FID;
	RR->OffsetLow = (u32)(fileoffset & 0xffffffff);
	RR->OffsetHigh = (u32)((fileoffset >> 32) & 0xffffffff);
	RR->MaxCountLow = nbytes;

	nb_SetSessionMessage(sizeof(ReadAndXRequest_t));
	r = GetSMBServerReply();
	if (r <= 0)
		return -EIO;

	// check sanity of SMB header
	if (RRsp->smbH.Magic != SMB_MAGIC)
		return -EIO;

	// check there's no error
	if ((RRsp->smbH.Eclass | (RRsp->smbH.Ecode << 16)) != STATUS_SUCCESS)
		return -EIO;

	r = RRsp->DataLengthLow;

	if (RRsp->DataOffset > 0)
		memcpy(readbuf, &SMB_buf.u8buff[4 + RRsp->DataOffset], r);

	return r;
}

//-------------------------------------------------------------------------
int smb_WriteAndX(int UID, int TID, int FID, s64 fileoffset, void *writebuf, u16 nbytes)
{
	int r;
	WriteAndXRequest_t *WR = &SMB_buf.pkt.writeAndXRequest;
	WriteAndXResponse_t *WRsp = &SMB_buf.pkt.writeAndXResponse;

	memcpy(WR, &smb_Write_Request, sizeof(WriteAndXRequest_t));

	WR->smbH.UID = (u16)UID;
	WR->smbH.TID = (u16)TID;
	WR->FID = (u16)FID;
	WR->OffsetLow = (u32)(fileoffset & 0xffffffff);
	WR->OffsetHigh = (u32)((fileoffset >> 32) & 0xffffffff);
	WR->Remaining = nbytes;
	WR->DataLengthLow = nbytes;
	WR->ByteCount = nbytes;

	memcpy((void *)(&SMB_buf.u8buff[4 + WR->DataOffset]), writebuf, nbytes);

	nb_SetSessionMessage(sizeof(WriteAndXRequest_t) + nbytes);
	r = GetSMBServerReply();
	if (r <= 0)
		return -EIO;

	// check sanity of SMB header
	if (WRsp->smbH.Magic != SMB_MAGIC)
		return -EIO;

	// check there's no error
	if ((WRsp->smbH.Eclass | (WRsp->smbH.Ecode << 16)) != STATUS_SUCCESS)
		return -EIO;

	return nbytes;
}

//-------------------------------------------------------------------------
int smb_Close(int UID, int TID, int FID)
{
	int r;
	CloseRequest_t *CR = &SMB_buf.pkt.closeRequest;
	CloseResponse_t *CRsp = &SMB_buf.pkt.closeResponse;

	memset(CR, 0, sizeof(CloseRequest_t));

	CR->smbH.Magic = SMB_MAGIC;
	CR->smbH.Cmd = SMB_COM_CLOSE;
	CR->smbH.Flags = SMB_FLAGS_CANONICAL_PATHNAMES;
	CR->smbH.Flags2 = SMB_FLAGS2_KNOWS_LONG_NAMES | SMB_FLAGS2_32BIT_STATUS;
	CR->smbH.UID = (u16)UID;
	CR->smbH.TID = (u16)TID;
	CR->smbWordcount = 3;
	CR->FID = (u16)FID;

	nb_SetSessionMessage(sizeof(CloseRequest_t));
	r = GetSMBServerReply();
	if (r <= 0)
		return -EIO;

	// check sanity of SMB header
	if (CRsp->smbH.Magic != SMB_MAGIC)
		return -EIO;

	// check there's no error
	if ((CRsp->smbH.Eclass | (CRsp->smbH.Ecode << 16)) != STATUS_SUCCESS)
		return -EIO;

	return 0;
}

//-------------------------------------------------------------------------
int smb_Delete(int UID, int TID, char *Path)
{
	int r, CF, PathLen;
	DeleteRequest_t *DR = &SMB_buf.pkt.deleteRequest;
	DeleteResponse_t *DRsp = &SMB_buf.pkt.deleteResponse;

	memset(DR, 0, sizeof(DeleteRequest_t));

	CF = server_specs.StringsCF;

	DR->smbH.Magic = SMB_MAGIC;
	DR->smbH.Cmd = SMB_COM_DELETE;
	DR->smbH.Flags = SMB_FLAGS_CANONICAL_PATHNAMES;
	DR->smbH.Flags2 = SMB_FLAGS2_KNOWS_LONG_NAMES | SMB_FLAGS2_32BIT_STATUS;
	if (CF == 2)
		DR->smbH.Flags2 |= SMB_FLAGS2_UNICODE_STRING;
	DR->smbH.UID = (u16)UID;
	DR->smbH.TID = (u16)TID;
	DR->smbWordcount = 1;
	DR->SearchAttributes = 0; // coud be other attributes to find Hidden/System files
	DR->BufferFormat = 0x04;

	// Add path
	PathLen = setStringField(DR->FileName, Path);
	DR->ByteCount = PathLen+1; 			// +1 for the BufferFormat byte

	nb_SetSessionMessage(sizeof(DeleteRequest_t) + PathLen);
	r = GetSMBServerReply();
	if (r <= 0)
		return -EIO;

	// check sanity of SMB header
	if (DRsp->smbH.Magic != SMB_MAGIC)
		return -EIO;

	// check there's no error
	if ((DRsp->smbH.Eclass | (DRsp->smbH.Ecode << 16)) != STATUS_SUCCESS)
		return -EIO;

	return 0;
}

//-------------------------------------------------------------------------
int smb_ManageDirectory(int UID, int TID, char *Path, int cmd)
{
	int r, CF, PathLen;
	ManageDirectoryRequest_t *MDR = &SMB_buf.pkt.manageDirectoryRequest;
	ManageDirectoryResponse_t *MDRsp = &SMB_buf.pkt.manageDirectoryResponse;

	memset(MDR, 0, sizeof(ManageDirectoryRequest_t));

	CF = server_specs.StringsCF;

	MDR->smbH.Magic = SMB_MAGIC;

	MDR->smbH.Cmd = (u8)cmd;
	MDR->smbH.Flags = SMB_FLAGS_CANONICAL_PATHNAMES;
	MDR->smbH.Flags2 = SMB_FLAGS2_KNOWS_LONG_NAMES | SMB_FLAGS2_32BIT_STATUS;
	if (CF == 2)
		MDR->smbH.Flags2 |= SMB_FLAGS2_UNICODE_STRING;
	MDR->smbH.UID = (u16)UID;
	MDR->smbH.TID = (u16)TID;
	MDR->BufferFormat = 0x04;

	// Add path
	PathLen = setStringField(MDR->DirectoryName, Path);
	MDR->ByteCount = PathLen+1; 			// +1 for the BufferFormat byte

	nb_SetSessionMessage(sizeof(ManageDirectoryRequest_t)+PathLen);
	r = GetSMBServerReply();
	if (r <= 0)
		return -EIO;

	// check sanity of SMB header
	if (MDRsp->smbH.Magic != SMB_MAGIC)
		return -EIO;

	switch(MDRsp->smbH.Eclass | (MDRsp->smbH.Ecode << 16)){
		// check there's no error
		case STATUS_SUCCESS:
			break;
		// check if access denied
		case STATUS_ACCESS_DENIED:
			return -EACCES;
		default:
			return -EIO;
	}

	return 0;
}

//-------------------------------------------------------------------------
int smb_Rename(int UID, int TID, char *oldPath, char *newPath)
{
	int r, CF, offset;
	RenameRequest_t *RR = &SMB_buf.pkt.renameRequest;
	RenameResponse_t *RRsp = &SMB_buf.pkt.renameResponse;

	memset(RR, 0, sizeof(RenameRequest_t));

	CF = server_specs.StringsCF;

	RR->smbH.Magic = SMB_MAGIC;
	RR->smbH.Cmd = SMB_COM_RENAME;
	RR->smbH.Flags = SMB_FLAGS_CANONICAL_PATHNAMES;
	RR->smbH.Flags2 = SMB_FLAGS2_KNOWS_LONG_NAMES | SMB_FLAGS2_32BIT_STATUS;
	if (CF == 2)
		RR->smbH.Flags2 |= SMB_FLAGS2_UNICODE_STRING;
	RR->smbH.UID = (u16)UID;
	RR->smbH.TID = (u16)TID;
	RR->smbWordcount = 1;

	// NOTE: on samba seems it doesn't care of attribute to rename directories
	// to be tested on windows
	RR->SearchAttributes = 0; // coud be other attributes to find Hidden/System files /Directories

	offset = 0;

	// Add oldPath
	RR->ByteField[offset++] = 0x04;			// BufferFormat
	offset += setStringField(&RR->ByteField[offset], oldPath);

	// Add newPath
	RR->ByteField[offset++] = 0x04;			// BufferFormat
	if (CF == 2)
	{
		RR->ByteField[offset] = '\0';
		offset++;				// pad needed for unicode
	}
	offset += setStringField(&RR->ByteField[offset], newPath);

	RR->ByteCount = offset;

	nb_SetSessionMessage(sizeof(RenameRequest_t) + offset);
	r = GetSMBServerReply();
	if (r <= 0)
		return -EIO;

	// check sanity of SMB header
	if (RRsp->smbH.Magic != SMB_MAGIC)
		return -EIO;

	// check there's no error
	switch(RRsp->smbH.Eclass | (RRsp->smbH.Ecode << 16)){
		// check there's no error
		case STATUS_SUCCESS:
			break;
		// check if access denied
		case STATUS_ACCESS_DENIED:
			return -EACCES;
		default:
			return -EIO;
	}

	return 0;
}

//-------------------------------------------------------------------------
int smb_FindFirstNext2(int UID, int TID, char *Path, int cmd, SearchInfo_t *info)
{
	int r, CF, PathLen, offset;
	FindFirstNext2Request_t *FFNR = &SMB_buf.pkt.findFirstNext2Request;
	FindFirstNext2Response_t *FFNRsp = &SMB_buf.pkt.findFirstNext2Response;

	memset(FFNR, 0, sizeof(FindFirstNext2Request_t));

	CF = server_specs.StringsCF;

	FFNR->smbH.Magic = SMB_MAGIC;
	FFNR->smbH.Cmd = SMB_COM_TRANSACTION2;
	FFNR->smbH.Flags = SMB_FLAGS_CANONICAL_PATHNAMES;
	FFNR->smbH.Flags2 = SMB_FLAGS2_KNOWS_LONG_NAMES | SMB_FLAGS2_32BIT_STATUS;
	if (CF == 2)
		FFNR->smbH.Flags2 |= SMB_FLAGS2_UNICODE_STRING;
	FFNR->smbH.UID = (u16)UID;
	FFNR->smbH.TID = (u16)TID;
	FFNR->smbWordcount = 15;

	FFNR->smbTrans.SetupCount = 1;
	FFNR->SubCommand = (u8)cmd;

	FFNR->smbTrans.ParamOffset = (sizeof(FindFirstNext2Request_t) + 3) & ~3; //Keep aligned to a 4-byte boundary as required.
	FFNR->smbTrans.MaxParamCount = 256; 		// Max Parameters len in reply
	FFNR->smbTrans.MaxDataCount = 16384;		// Max Data len in reply

	//Zero Pad1.
	memset((void*)(FFNR + 1), 0, FFNR->smbTrans.ParamOffset - sizeof(FindFirstNext2Request_t));

	if (cmd == TRANS2_FIND_FIRST2) {
		FindFirst2RequestParam_t *FFRParam = (FindFirst2RequestParam_t *)&SMB_buf.u8buff[FFNR->smbTrans.ParamOffset+4]; //+4 to skip the session header.

		FFRParam->SearchAttributes = ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_DIRECTORY | ATTR_ARCHIVE;
		FFRParam->SearchCount = 1;
		FFRParam->Flags = CLOSE_SEARCH_IF_EOS | RESUME_SEARCH;
		FFRParam->LevelOfInterest = SMB_FIND_FILE_BOTH_DIRECTORY_INFO;
		FFRParam->StorageType = 0;

		// Add path
		PathLen = setStringField(FFRParam->SearchPattern, Path);

		FFNR->smbTrans.TotalParamCount = FFNR->smbTrans.ParamCount = sizeof(FindFirst2RequestParam_t) + PathLen;
	}
	else {
		FindNext2RequestParam_t *FNRParam = (FindNext2RequestParam_t *)&SMB_buf.u8buff[FFNR->smbTrans.ParamOffset+4]; //+4 to skip the session header.

		FNRParam->SearchID = (u16)info->SID;
		FNRParam->SearchCount = 1;
		FNRParam->LevelOfInterest = SMB_FIND_FILE_BOTH_DIRECTORY_INFO;
		FNRParam->ResumeKey = 0;
		FNRParam->Flags = CLOSE_SEARCH_IF_EOS | RESUME_SEARCH | CONTINUE_SEARCH;
		FNRParam->SearchPattern[0] = 0;
		FFNR->smbTrans.TotalParamCount = FFNR->smbTrans.ParamCount = sizeof(FindNext2RequestParam_t) + 1;
	}

	FFNR->ByteCount = 3 + FFNR->smbTrans.TotalParamCount;
	offset = FFNR->smbTrans.ParamOffset + FFNR->smbTrans.TotalParamCount;

	//No data, so no Pad2. As DataCount is 0, the client may set DataOffset to 0.
	//FFNR->smbTrans.DataOffset = (u16)offset;

	nb_SetSessionMessage(offset);
	r = GetSMBServerReply();
	if (r <= 0)
		return -EIO;

	// check sanity of SMB header
	if (FFNRsp->smbH.Magic != SMB_MAGIC)
		return -EIO;

	// check there's no error
	switch(FFNRsp->smbH.Eclass | (FFNRsp->smbH.Ecode << 16)){
		// check there's no error
		case STATUS_SUCCESS:
			break;
		// check if access denied
		case STATUS_ACCESS_DENIED:
			return -EACCES;
		default:
			return -EIO;
	}

	FindFirstNext2ResponseParam_t *FFNRspParam;

	if (cmd == TRANS2_FIND_FIRST2) {
		FFNRspParam = (FindFirstNext2ResponseParam_t *)&SMB_buf.u8buff[FFNRsp->smbTrans.ParamOffset+4];
		info->SID = FFNRspParam->SearchID;
	}
	else
		FFNRspParam = (FindFirstNext2ResponseParam_t *)&SMB_buf.u8buff[FFNRsp->smbTrans.ParamOffset+4-2];

	FindFirst2ResponseData_t *FFRspData = (FindFirst2ResponseData_t *)&SMB_buf.u8buff[FFNRsp->smbTrans.DataOffset+4];

	info->EOS = FFNRspParam->EndOfSearch;

	if (FFNRspParam->SearchCount == 0)
		return -EINVAL;

	info->fileInfo.Created = FFRspData->Created;
	info->fileInfo.LastAccess = FFRspData->LastAccess;
	info->fileInfo.LastWrite = FFRspData->LastWrite;
	info->fileInfo.Change = FFRspData->Change;
	info->fileInfo.FileAttributes = FFRspData->FileAttributes;
	if (FFRspData->FileAttributes & EXT_ATTR_DIRECTORY)
		info->fileInfo.IsDirectory = 1;
	info->fileInfo.AllocationSize = FFRspData->AllocationSize;
	info->fileInfo.EndOfFile = FFRspData->EndOfFile;
	getStringField(info->FileName, FFRspData->FileName);

	return 0;
}

//-------------------------------------------------------------------------
int smb_TreeDisconnect(int UID, int TID)
{
	int r;
	TreeDisconnectRequest_t *TDR = &SMB_buf.pkt.treeDisconnectRequest;
	TreeDisconnectResponse_t *TDRsp = &SMB_buf.pkt.treeDisconnectResponse;

	memset(TDR, 0, sizeof(TreeDisconnectRequest_t));

	TDR->smbH.Magic = SMB_MAGIC;
	TDR->smbH.Cmd = SMB_COM_TREE_DISCONNECT;
	TDR->smbH.UID = (u16)UID;
	TDR->smbH.TID = (u16)TID;

	nb_SetSessionMessage(sizeof(TreeDisconnectRequest_t));
	r = GetSMBServerReply();
	if (r <= 0)
		return -EIO;

	// check sanity of SMB header
	if (TDRsp->smbH.Magic != SMB_MAGIC)
		return -EIO;

	// check there's no error
	if ((TDRsp->smbH.Eclass | (TDRsp->smbH.Ecode << 16)) != STATUS_SUCCESS)
		return -EIO;

	TID = -1;

	return 0;
}

//-------------------------------------------------------------------------
int smb_LogOffAndX(int UID)
{
	int r;
	LogOffAndXRequest_t *LR = &SMB_buf.pkt.logOffAndXRequest;
	LogOffAndXResponse_t *LRsp = &SMB_buf.pkt.logOffAndXResponse;

	memset(LR, 0, sizeof(LogOffAndXRequest_t));

	LR->smbH.Magic = SMB_MAGIC;
	LR->smbH.Cmd = SMB_COM_LOGOFF_ANDX;
	LR->smbH.UID = (u16)UID;
	LR->smbWordcount = 2;
	LR->smbAndxCmd = SMB_COM_NONE;		// no ANDX command

	nb_SetSessionMessage(sizeof(LogOffAndXRequest_t));
	r = GetSMBServerReply();
	if (r <= 0)
		return -EIO;

	// check sanity of SMB header
	if (LRsp->smbH.Magic != SMB_MAGIC)
		return -EIO;

	// check there's no error
	if ((LRsp->smbH.Eclass | (LRsp->smbH.Ecode << 16)) != STATUS_SUCCESS)
		return -EIO;

	UID = -1;

	return 0;
}

//-------------------------------------------------------------------------
int smb_Echo(void *echo, int len)
{
	int r;
	EchoRequest_t *ER = &SMB_buf.pkt.echoRequest;
	EchoResponse_t *ERsp = &SMB_buf.pkt.echoResponse;

	memset(ER, 0, sizeof(EchoRequest_t));

	ER->smbH.Magic = SMB_MAGIC;
	ER->smbH.Cmd = SMB_COM_ECHO;
	ER->smbWordcount = 1;
	ER->EchoCount = 1;

	memcpy(&ER->ByteField[0], echo, (u16)len);
	ER->ByteCount = (u16)len;

	nb_SetSessionMessage(sizeof(EchoRequest_t)+(u16)len);
	r = GetSMBServerReply();
	if (r <= 0)
		return -EIO;

	// check sanity of SMB header
	if (ERsp->smbH.Magic != SMB_MAGIC)
		return -EIO;

	// check there's no error
	if ((ERsp->smbH.Eclass | (ERsp->smbH.Ecode << 16)) != STATUS_SUCCESS)
		return -EIO;

	if (memcmp(&ERsp->ByteField[0], echo, len))
		return -EIO;

	return 0;
}

//-------------------------------------------------------------------------
int smb_Connect(char *SMBServerIP, int SMBServerPort)
{
	int r;
	struct in_addr dst_addr;

	dst_addr.s_addr = inet_addr(SMBServerIP);

	// Close the connection if it was already opened
	smb_Disconnect();

	// Opening TCP session
	r = OpenTCPSession(dst_addr, SMBServerPort, &main_socket);
	if (r < 0) {
		return -1;
	}

	// We keep the server IP for SMB logon
	strncpy(server_specs.ServerIP, SMBServerIP, 16);

	return 0;
}

//-------------------------------------------------------------------------
int smb_Disconnect(void)
{
	char dummy;

	if (main_socket != -1) {
		//Ensure that all data has been received by the other side before closing.
		shutdown(main_socket, SHUT_WR);
		//Wait for the remote end to close the socket, which shall happen after all sent data has been received.
		while(recv(main_socket, &dummy, sizeof(dummy), 0) > 0);

		lwip_close(main_socket);
		main_socket = -1;
	}

	return 0;
}
