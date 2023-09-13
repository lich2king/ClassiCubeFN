#include "SSL.h"
#include "Errors.h"

#if defined CC_BUILD_SCHANNEL
#define WIN32_LEAN_AND_MEAN
#define NOSERVICE
#define NOMCX
#define NOIME
#include <windows.h>
#define SECURITY_WIN32
#include <sspi.h>
#include <schannel.h>
#include "Platform.h"
#include "String.h"
#include "Funcs.h"

/* https://gist.github.com/mmozeiko/c0dfcc8fec527a90a02145d2cc0bfb6d */
/* https://web.archive.org/web/20210116110926/http://www.coastrd.com/c-schannel-smtp */

/* https://hpbn.co/transport-layer-security-tls/ */
#define TLS_MAX_PACKET_SIZE (16384 + 512) /* 16kb record size + header/mac/padding */
/* TODO: Check against sizes.cbMaximumMessage */

static void* schannel_lib;
static INIT_SECURITY_INTERFACE_A _InitSecurityInterfaceA;
static cc_bool _verifyCerts;

static ACQUIRE_CREDENTIALS_HANDLE_FN_A  FP_AcquireCredentialsHandleA;
static FREE_CREDENTIALS_HANDLE_FN       FP_FreeCredentialsHandle;
static INITIALIZE_SECURITY_CONTEXT_FN_A FP_InitializeSecurityContextA;
static ACCEPT_SECURITY_CONTEXT_FN       FP_AcceptSecurityContext;
static COMPLETE_AUTH_TOKEN_FN           FP_CompleteAuthToken;
static DELETE_SECURITY_CONTEXT_FN       FP_DeleteSecurityContext;
static QUERY_CONTEXT_ATTRIBUTES_FN_A    FP_QueryContextAttributesA;
static FREE_CONTEXT_BUFFER_FN           FP_FreeContextBuffer;
static ENCRYPT_MESSAGE_FN               FP_EncryptMessage;
static DECRYPT_MESSAGE_FN               FP_DecryptMessage;

void SSLBackend_Init(cc_bool verifyCerts) {
	/* secur32.dll is available on Win9x and later */
	/* Security.dll is available on NT 4 and later */

	/* Officially, InitSecurityInterfaceA and then AcquireCredentialsA from */
	/*  secur32.dll (or security.dll) should be called - however */
	/*  AcquireCredentialsA fails with SEC_E_SECPKG_NOT_FOUND on Win 9x */
	/* But if you instead directly call those functions from schannel.dll, */
	/*  then it DOES work. (and on later Windows versions, those functions */
	/*  exported from schannel.dll are just DLL forwards to secur32.dll */
	static const struct DynamicLibSym funcs[] = {
		DynamicLib_Sym(InitSecurityInterfaceA)
	};
	static const cc_string schannel = String_FromConst("schannel.dll");
	_verifyCerts = verifyCerts;
	/* TODO: Load later?? it's unsafe to do on a background thread though */
	DynamicLib_LoadAll(&schannel, funcs, Array_Elems(funcs), &schannel_lib);
}

cc_bool SSLBackend_DescribeError(cc_result res, cc_string* dst) {
	switch (res) {
	case SEC_E_UNTRUSTED_ROOT:
		String_AppendConst(dst, "The website's SSL certificate was issued by an authority that is not trusted");
		return true;
	case SEC_E_CERT_EXPIRED:
		String_AppendConst(dst, "The website's SSL certificate has expired");
		return true;
	case TRUST_E_CERT_SIGNATURE:
		String_AppendConst(dst, "The signature of the website's SSL certificate cannot be verified");
		return true;
	case SEC_E_UNSUPPORTED_FUNCTION:
		/* https://learn.microsoft.com/en-us/windows/win32/secauthn/schannel-error-codes-for-tls-and-ssl-alerts */
		/* TLS1_ALERT_PROTOCOL_VERSION maps to this error code */
		String_AppendConst(dst, "The website uses an incompatible SSL/TLS version");
		return true;
	}
	return false;
}


struct SSLContext {
	cc_socket socket;
	CredHandle handle;
	CtxtHandle context;
	SecPkgContext_StreamSizes sizes;
	DWORD flags;
	int bufferLen;
	int leftover; /* number of unprocessed bytes leftover from last successful DecryptMessage */
	int decryptedSize;
	char* decryptedData;
	char incoming[TLS_MAX_PACKET_SIZE];
};

/* Undefined in older MinGW versions */
#define _SP_PROT_TLS1_1_CLIENT 0x00000200
#define _SP_PROT_TLS1_2_CLIENT 0x00000800

static SECURITY_STATUS SSL_CreateHandle(struct SSLContext* ctx) {
	SCHANNEL_CRED cred = { 0 };
	cred.dwVersion = SCHANNEL_CRED_VERSION;
	cred.dwFlags   = SCH_CRED_NO_DEFAULT_CREDS | (_verifyCerts ? SCH_CRED_AUTO_CRED_VALIDATION : SCH_CRED_MANUAL_CRED_VALIDATION);
	cred.grbitEnabledProtocols = SP_PROT_TLS1_CLIENT | _SP_PROT_TLS1_1_CLIENT | _SP_PROT_TLS1_2_CLIENT;

	/* TODO: SCHANNEL_NAME_A ? */
	return FP_AcquireCredentialsHandleA(NULL, UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL,
						&cred, NULL, NULL, &ctx->handle, NULL);
}

static cc_result SSL_SendRaw(cc_socket socket, const cc_uint8* data, cc_uint32 count) {
	cc_uint32 sent;
	cc_result res;

	while (count)
	{
		if ((res = Socket_Write(socket, data, count, &sent))) return res;
		if (!sent) return ERR_END_OF_STREAM;

		data  += sent;
		count -= sent;
	}
	return 0;
}

static cc_result SSL_RecvRaw(struct SSLContext* ctx) {
	cc_uint32 read;
	cc_result res;
	
	/* server is sending too much garbage data instead of proper TLS packets ?? */
	if (ctx->bufferLen == sizeof(ctx->incoming)) return ERR_INVALID_ARGUMENT;

	res = Socket_Read(ctx->socket, ctx->incoming + ctx->bufferLen,
						sizeof(ctx->incoming) - ctx->bufferLen, &read);

	if (res)   return res;
	if (!read) return ERR_END_OF_STREAM;

	ctx->bufferLen += read;
	return 0;
}


/* Sends the initial TLS handshake ClientHello message to the server */
static SECURITY_STATUS SSL_Connect(struct SSLContext* ctx, const char* hostname) {
	SecBuffer out_buffers[1];
	SecBufferDesc out_desc;
	SECURITY_STATUS res;
	DWORD flags = ctx->flags;

	out_buffers[0].BufferType = SECBUFFER_TOKEN;
	out_buffers[0].pvBuffer   = NULL;
	out_buffers[0].cbBuffer   = 0;

	out_desc.ulVersion = SECBUFFER_VERSION;
	out_desc.cBuffers  = Array_Elems(out_buffers);
	out_desc.pBuffers  = out_buffers;

	res = FP_InitializeSecurityContextA(&ctx->handle, NULL, hostname, flags, 0, 0,
						NULL, 0, &ctx->context, &out_desc, &flags, NULL);
	if (res != SEC_I_CONTINUE_NEEDED) return res;
	res = 0;

	/* Send initial handshake to the server (if there is one) */
	if (out_buffers[0].pvBuffer) {
		res = SSL_SendRaw(ctx->socket, out_buffers[0].pvBuffer, out_buffers[0].cbBuffer);
		FP_FreeContextBuffer(out_buffers[0].pvBuffer);
	}
	return res;
}

/* Performs (Negotiates) the rest of the TLS handshake */
static SECURITY_STATUS SSL_Negotiate(struct SSLContext* ctx) {
	SecBuffer in_buffers[2];
	SecBuffer out_buffers[1];
	SecBufferDesc in_desc;
	SecBufferDesc out_desc;
	cc_uint32 leftover_len;
	SECURITY_STATUS sec;
	cc_result res;
	DWORD flags;

	for (;;)
	{
		/* buffer 0 = data received from server which SChannel processes */
		/* buffer 1 = any leftover data which SChannel didn't process this time */
		/*  (this data must be persisted, as it will be used next time around) */
		in_buffers[0].BufferType = SECBUFFER_TOKEN;
		in_buffers[0].pvBuffer   = ctx->incoming;
		in_buffers[0].cbBuffer   = ctx->bufferLen;
		in_buffers[1].BufferType = SECBUFFER_EMPTY;
		in_buffers[1].pvBuffer   = NULL;
		in_buffers[1].cbBuffer   = 0;

		out_buffers[0].BufferType = SECBUFFER_TOKEN;
		out_buffers[0].pvBuffer   = NULL;
		out_buffers[0].cbBuffer   = 0;

		in_desc.ulVersion  = SECBUFFER_VERSION;
		in_desc.cBuffers   = Array_Elems(in_buffers);
		in_desc.pBuffers   = in_buffers;

		out_desc.ulVersion = SECBUFFER_VERSION;
		out_desc.cBuffers  = Array_Elems(out_buffers);
		out_desc.pBuffers  = out_buffers;

		flags = ctx->flags;
		sec   = FP_InitializeSecurityContextA(&ctx->handle, &ctx->context, NULL, flags, 0, 0,
							&in_desc, 0, NULL, &out_desc, &flags, NULL);

		if (in_buffers[1].BufferType == SECBUFFER_EXTRA) {
			/* SChannel didn't process the entirety of the input buffer */
			/*  So move the leftover data back to the front of the input buffer */
			leftover_len = in_buffers[1].cbBuffer;
			memmove(ctx->incoming, ctx->incoming + (ctx->bufferLen - leftover_len), leftover_len);
			ctx->bufferLen = leftover_len;
		} else if (sec != SEC_E_INCOMPLETE_MESSAGE) {
			/* SChannel processed entirely of input buffer */
			ctx->bufferLen = 0;
		}

		/* Handshake completed */
		if (sec == SEC_E_OK) break;

		/* Need to send data to the server */
		if (sec == SEC_I_CONTINUE_NEEDED) {
			res = SSL_SendRaw(ctx->socket, out_buffers[0].pvBuffer, out_buffers[0].cbBuffer);
			FP_FreeContextBuffer(out_buffers[0].pvBuffer); /* TODO always free? */

			if (res) return res;
			continue;
		}
		
		if (sec != SEC_E_INCOMPLETE_MESSAGE) return sec;
		/* SEC_E_INCOMPLETE_MESSAGE case - need to read more data from the server first */
		if ((res = SSL_RecvRaw(ctx))) return res;
	}

	FP_QueryContextAttributesA(&ctx->context, SECPKG_ATTR_STREAM_SIZES, &ctx->sizes);
	return 0;
}


static void SSL_LoadSecurityFunctions(PSecurityFunctionTableA sspiFPs) {
	FP_AcquireCredentialsHandleA  = sspiFPs->AcquireCredentialsHandleA;
	FP_FreeCredentialsHandle      = sspiFPs->FreeCredentialsHandle;
	FP_InitializeSecurityContextA = sspiFPs->InitializeSecurityContextA;
	FP_AcceptSecurityContext      = sspiFPs->AcceptSecurityContext;
	FP_CompleteAuthToken          = sspiFPs->CompleteAuthToken;
	FP_DeleteSecurityContext      = sspiFPs->DeleteSecurityContext;
	FP_QueryContextAttributesA    = sspiFPs->QueryContextAttributesA;
	FP_FreeContextBuffer          = sspiFPs->FreeContextBuffer;

	FP_EncryptMessage = sspiFPs->EncryptMessage;
	FP_DecryptMessage = sspiFPs->DecryptMessage;
	/* Old Windows versions don't have EncryptMessage/DecryptMessage, */
	/*  but have the older SealMessage/UnsealMessage functions instead */
	if (!FP_EncryptMessage) FP_EncryptMessage = (ENCRYPT_MESSAGE_FN)sspiFPs->Reserved3;
	if (!FP_DecryptMessage) FP_DecryptMessage = (DECRYPT_MESSAGE_FN)sspiFPs->Reserved4;
}

cc_result SSL_Init(cc_socket socket, const cc_string* host_, void** out_ctx) {
	PSecurityFunctionTableA sspiFPs;
	struct SSLContext* ctx;
	SECURITY_STATUS res;
	cc_winstring host;
	if (!_InitSecurityInterfaceA) return HTTP_ERR_NO_SSL;

	if (!FP_InitializeSecurityContextA) {
		sspiFPs = _InitSecurityInterfaceA();
		if (!sspiFPs) return ERR_NOT_SUPPORTED;
		SSL_LoadSecurityFunctions(sspiFPs);
	}

	ctx = Mem_TryAllocCleared(1, sizeof(struct SSLContext));
	if (!ctx) return ERR_OUT_OF_MEMORY;
	*out_ctx = (void*)ctx;

	ctx->flags = ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT | ISC_REQ_USE_SUPPLIED_CREDS | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
	if (!_verifyCerts) ctx->flags |= ISC_REQ_MANUAL_CRED_VALIDATION;

	ctx->socket = socket;
	Platform_EncodeString(&host, host_);

	if ((res = SSL_CreateHandle(ctx)))	     return res;
	if ((res = SSL_Connect(ctx, host.ansi))) return res;
	if ((res = SSL_Negotiate(ctx)))		     return res;
	return 0;
}


static cc_result SSL_ReadDecrypted(struct SSLContext* ctx, cc_uint8* data, cc_uint32 count, cc_uint32* read) {
	int len = min(count, ctx->decryptedSize);
	Mem_Copy(data, ctx->decryptedData, len);

	if (len == ctx->decryptedSize) {
		/* incoming buffer stores decrypted data and then any leftover ciphertext */
		/*  So move the leftover ciphertext back to the start of the input buffer */
		/* TODO: Share function with handshake function */
		memmove(ctx->incoming, ctx->incoming + (ctx->bufferLen - ctx->leftover), ctx->leftover);
		ctx->bufferLen = ctx->leftover;
		ctx->leftover  = 0;

		ctx->decryptedData = NULL;
		ctx->decryptedSize = 0;
	} else {
		ctx->decryptedData += len;
		ctx->decryptedSize -= len;
	}

	*read = len;
	return 0;
}

cc_result SSL_Read(void* ctx_, cc_uint8* data, cc_uint32 count, cc_uint32* read) {
	struct SSLContext* ctx = ctx_;
	SecBuffer buffers[4];
	SecBufferDesc desc;
	SECURITY_STATUS sec;
	cc_result res;

	/* decrypted data from previously */
	if (ctx->decryptedData) return SSL_ReadDecrypted(ctx, data, count, read);

	for (;;)
	{
		/* if any ciphertext data, then try to decrypt it */
		if (ctx->bufferLen) {
			/* https://learn.microsoft.com/en-us/windows/win32/secauthn/stream-contexts */
			buffers[0].BufferType = SECBUFFER_DATA;
			buffers[0].pvBuffer   = ctx->incoming;
			buffers[0].cbBuffer   = ctx->bufferLen;
			buffers[1].BufferType = SECBUFFER_EMPTY;
			buffers[2].BufferType = SECBUFFER_EMPTY;
			buffers[3].BufferType = SECBUFFER_EMPTY;

			desc.ulVersion = SECBUFFER_VERSION;
			desc.cBuffers  = Array_Elems(buffers);
			desc.pBuffers  = buffers;

			sec = FP_DecryptMessage(&ctx->context, &desc, 0, NULL);
			if (sec == SEC_E_OK) {				
				/* After successful decryption the SecBuffers will be: */
				/*   buffers[0] = headers */
				/*   buffers[1] = content */
				/*   buffers[2] = trailers */
				/*   buffers[3] = extra, if any leftover unprocessed data */
				ctx->decryptedData = buffers[1].pvBuffer;
				ctx->decryptedSize = buffers[1].cbBuffer;
				ctx->leftover	   = buffers[3].BufferType == SECBUFFER_EXTRA ? buffers[3].cbBuffer : 0;

				return SSL_ReadDecrypted(ctx, data, count, read);
			}
			
			if (sec != SEC_E_INCOMPLETE_MESSAGE) return sec;
			/* SEC_E_INCOMPLETE_MESSAGE case - still need to read more data from the server first */
		}

		/* not enough data received yet to decrypt, so need to read more data from the server */
		if ((res = SSL_RecvRaw(ctx))) return res;
	}
	return 0;
}

static cc_result SSL_WriteChunk(struct SSLContext* s, const cc_uint8* data, cc_uint32 count) {
	char buffer[TLS_MAX_PACKET_SIZE];
	SecBuffer buffers[3];
	SecBufferDesc desc;
	SECURITY_STATUS res;
	int total;

	/* https://learn.microsoft.com/en-us/windows/win32/secauthn/encryptmessage--schannel */
	buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
	buffers[0].pvBuffer   = buffer;
	buffers[0].cbBuffer   = s->sizes.cbHeader;
	buffers[1].BufferType = SECBUFFER_DATA;
	buffers[1].pvBuffer   = buffer + s->sizes.cbHeader;
	buffers[1].cbBuffer   = count;
	buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
	buffers[2].pvBuffer   = buffer + s->sizes.cbHeader + count;
	buffers[2].cbBuffer   = s->sizes.cbTrailer;

	/* See https://learn.microsoft.com/en-us/windows/win32/api/sspi/nf-sspi-encryptmessage */
	/*  ".. The message is encrypted in place, overwriting the original contents of the structure" */
	Mem_Copy(buffers[1].pvBuffer, data, count);
	
	desc.ulVersion = SECBUFFER_VERSION;
	desc.cBuffers  = Array_Elems(buffers);
	desc.pBuffers  = buffers;
	if ((res = FP_EncryptMessage(&s->context, 0, &desc, 0))) return res;

	/* NOTE: Okay to write in one go, since all three buffers will be contiguous */
	/*  (as TLS record header size will always be the same size) */
	total = buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer;
	return SSL_SendRaw(s->socket, buffer, total);
}

cc_result SSL_Write(void* ctx, const cc_uint8* data, cc_uint32 count, cc_uint32* wrote) {
	struct SSLContext* s = ctx;
	cc_result res;
	*wrote = 0;

	/* TODO: Don't loop here? move to HTTPConnection instead?? */
	while (count)
	{
		int len = min(count, s->sizes.cbMaximumMessage);
		if ((res = SSL_WriteChunk(s, data, len))) return res;

		*wrote += len;
		data   += len;
		count  -= len;
	}
	return 0;
}

cc_result SSL_Free(void* ctx_) {
	/* TODO send TLS close */
	struct SSLContext* ctx = (struct SSLContext*)ctx_;
	FP_DeleteSecurityContext(&ctx->context);
	FP_FreeCredentialsHandle(&ctx->handle);
	Mem_Free(ctx);
	return 0; 
}
#elif defined CC_BUILD_BEARSSL
#include "bearssl.h"
#define CERT_ATTRIBUTES
#include "../misc/RootCerts.h"
#include "String.h"
// https://github.com/unkaktus/bearssl/blob/master/samples/client_basic.c#L283

typedef struct SSLContext {
	br_ssl_client_context sc;
	br_x509_minimal_context xc;
	unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
	br_sslio_context ioc;
} SSLContext;

static cc_bool _verifyCerts;


void SSLBackend_Init(cc_bool verifyCerts) {
	_verifyCerts = verifyCerts; // TODO support
}
cc_bool SSLBackend_DescribeError(cc_result res, cc_string* dst) { return false; }

static int sock_read(void *ctx, unsigned char *buf, size_t len) {
	cc_uint32 read;
	cc_result res = Socket_Read((int)ctx, buf, len, &read);
	
	if (res) return -1;
	return read;
}
static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
	cc_uint32 wrote;
	cc_result res = Socket_Write((int)ctx, buf, len, &wrote);
	
	if (res) return -1;
	return wrote;
}
/*
 * The hardcoded trust anchors. These are the two DN + public key that
 * correspond to the self-signed certificates cert-root-rsa.pem and
 * cert-root-ec.pem.
 *
 * C code for hardcoded trust anchors can be generated with the "brssl"
 * command-line tool (with the "ta" command).
 */
static const unsigned char TA0_DN[] = {
	0x30, 0x1C, 0x31, 0x0B, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x06, 0x13,
	0x02, 0x43, 0x41, 0x31, 0x0D, 0x30, 0x0B, 0x06, 0x03, 0x55, 0x04, 0x03,
	0x13, 0x04, 0x52, 0x6F, 0x6F, 0x74
};
static const unsigned char TA0_RSA_N[] = {
	0xB6, 0xD9, 0x34, 0xD4, 0x50, 0xFD, 0xB3, 0xAF, 0x7A, 0x73, 0xF1, 0xCE,
	0x38, 0xBF, 0x5D, 0x6F, 0x45, 0xE1, 0xFD, 0x4E, 0xB1, 0x98, 0xC6, 0x60,
	0x83, 0x26, 0xD2, 0x17, 0xD1, 0xC5, 0xB7, 0x9A, 0xA3, 0xC1, 0xDE, 0x63,
	0x39, 0x97, 0x9C, 0xF0, 0x5E, 0x5C, 0xC8, 0x1C, 0x17, 0xB9, 0x88, 0x19,
	0x6D, 0xF0, 0xB6, 0x2E, 0x30, 0x50, 0xA1, 0x54, 0x6E, 0x93, 0xC0, 0xDB,
	0xCF, 0x30, 0xCB, 0x9F, 0x1E, 0x27, 0x79, 0xF1, 0xC3, 0x99, 0x52, 0x35,
	0xAA, 0x3D, 0xB6, 0xDF, 0xB0, 0xAD, 0x7C, 0xCB, 0x49, 0xCD, 0xC0, 0xED,
	0xE7, 0x66, 0x10, 0x2A, 0xE9, 0xCE, 0x28, 0x1F, 0x21, 0x50, 0xFA, 0x77,
	0x4C, 0x2D, 0xDA, 0xEF, 0x3C, 0x58, 0xEB, 0x4E, 0xBF, 0xCE, 0xE9, 0xFB,
	0x1A, 0xDA, 0xA3, 0x83, 0xA3, 0xCD, 0xA3, 0xCA, 0x93, 0x80, 0xDC, 0xDA,
	0xF3, 0x17, 0xCC, 0x7A, 0xAB, 0x33, 0x80, 0x9C, 0xB2, 0xD4, 0x7F, 0x46,
	0x3F, 0xC5, 0x3C, 0xDC, 0x61, 0x94, 0xB7, 0x27, 0x29, 0x6E, 0x2A, 0xBC,
	0x5B, 0x09, 0x36, 0xD4, 0xC6, 0x3B, 0x0D, 0xEB, 0xBE, 0xCE, 0xDB, 0x1D,
	0x1C, 0xBC, 0x10, 0x6A, 0x71, 0x71, 0xB3, 0xF2, 0xCA, 0x28, 0x9A, 0x77,
	0xF2, 0x8A, 0xEC, 0x42, 0xEF, 0xB1, 0x4A, 0x8E, 0xE2, 0xF2, 0x1A, 0x32,
	0x2A, 0xCD, 0xC0, 0xA6, 0x46, 0x2C, 0x9A, 0xC2, 0x85, 0x37, 0x91, 0x7F,
	0x46, 0xA1, 0x93, 0x81, 0xA1, 0x74, 0x66, 0xDF, 0xBA, 0xB3, 0x39, 0x20,
	0x91, 0x93, 0xFA, 0x1D, 0xA1, 0xA8, 0x85, 0xE7, 0xE4, 0xF9, 0x07, 0xF6,
	0x10, 0xF6, 0xA8, 0x27, 0x01, 0xB6, 0x7F, 0x12, 0xC3, 0x40, 0xC3, 0xC9,
	0xE2, 0xB0, 0xAB, 0x49, 0x18, 0x3A, 0x64, 0xB6, 0x59, 0xB7, 0x95, 0xB5,
	0x96, 0x36, 0xDF, 0x22, 0x69, 0xAA, 0x72, 0x6A, 0x54, 0x4E, 0x27, 0x29,
	0xA3, 0x0E, 0x97, 0x15
};
static const unsigned char TA0_RSA_E[] = {
	0x01, 0x00, 0x01
};
static const unsigned char TA1_DN[] = {
	0x30, 0x1C, 0x31, 0x0B, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x06, 0x13,
	0x02, 0x43, 0x41, 0x31, 0x0D, 0x30, 0x0B, 0x06, 0x03, 0x55, 0x04, 0x03,
	0x13, 0x04, 0x52, 0x6F, 0x6F, 0x74
};
static const unsigned char TA1_EC_Q[] = {
	0x04, 0x71, 0x74, 0xBA, 0xAB, 0xB9, 0x30, 0x2E, 0x81, 0xD5, 0xE5, 0x57,
	0xF9, 0xF3, 0x20, 0x68, 0x0C, 0x9C, 0xF9, 0x64, 0xDB, 0xB4, 0x20, 0x0D,
	0x6D, 0xEA, 0x40, 0xD0, 0x4A, 0x6E, 0x42, 0xFD, 0xB6, 0x9A, 0x68, 0x25,
	0x44, 0xF6, 0xDF, 0x7B, 0xC4, 0xFC, 0xDE, 0xDD, 0x7B, 0xBB, 0xC5, 0xDB,
	0x7C, 0x76, 0x3F, 0x41, 0x66, 0x40, 0x6E, 0xDB, 0xA7, 0x87, 0xC2, 0xE5,
	0xD8, 0xC5, 0xF3, 0x7F, 0x8D
};
static const br_x509_trust_anchor TAs[2] = {
	{
		{ (unsigned char *)TA0_DN, sizeof TA0_DN },
		BR_X509_TA_CA,
		{
			BR_KEYTYPE_RSA,
			{ .rsa = {
				(unsigned char *)TA0_RSA_N, sizeof TA0_RSA_N,
				(unsigned char *)TA0_RSA_E, sizeof TA0_RSA_E,
			} }
		}
	},
	{
		{ (unsigned char *)TA1_DN, sizeof TA1_DN },
		BR_X509_TA_CA,
		{
			BR_KEYTYPE_EC,
			{ .ec = {
				BR_EC_secp256r1,
				(unsigned char *)TA1_EC_Q, sizeof TA1_EC_Q,
			} }
		}
	}
};
#define TAs_NUM   2

cc_result SSL_Init(cc_socket socket, const cc_string* host_, void** out_ctx) {
	SSLContext* ctx;
	char host[NATIVE_STR_LEN];
	String_EncodeUtf8(host, host_);
	
	ctx = Mem_TryAlloc(1, sizeof(SSLContext));
	if (!ctx) return ERR_OUT_OF_MEMORY;
	*out_ctx = (void*)ctx;
	
	br_ssl_client_init_full(&ctx->sc, &ctx->xc, TAs, TAs_NUM);
	if (!_verify_certs) {
		br_x509_minimal_set_rsa(&ctx->xc,   &br_rsa_i31_pkcs1_vrfy);
		br_x509_minimal_set_ecdsa(&ctx->xc, &br_ec_prime_i31, &br_ecdsa_i31_vrfy_asn1);
	}
	br_ssl_engine_set_buffer(&ctx->sc.eng, ctx->iobuf, sizeof(ctx->iobuf), 1);
	br_ssl_client_reset(&ctx->sc, host, 0);
	
	br_sslio_init(&ctx->ioc, &ctx->sc.eng, 
			sock_read,  (void*)socket, 
			sock_write, (void*)socket);
	
	return 0;
}

cc_result SSL_Read(void* ctx_, cc_uint8* data, cc_uint32 count, cc_uint32* read) { 
	SSLContext* ctx = (SSLContext*)ctx_;
	// TODO: just br_sslio_write ??
	int res = br_sslio_read(&ctx->ioc, data, count);
	if (res < 0) return br_ssl_engine_last_error(&ctx->sc.eng);
	
	br_sslio_flush(&ctx->ioc);
	*read = res;
	return 0;
}

cc_result SSL_Write(void* ctx_, const cc_uint8* data, cc_uint32 count, cc_uint32* wrote) {
	SSLContext* ctx = (SSLContext*)ctx_;
	// TODO: just br_sslio_write ??
	int res = br_sslio_write_all(&ctx->ioc, data, count);
	if (res < 0) return br_ssl_engine_last_error(&ctx->sc.eng);
	
	br_sslio_flush(&ctx->ioc);
	*wrote = res;
	return 0;
}

cc_result SSL_Free(void* ctx_) {
	SSLContext* ctx = (SSLContext*)ctx_;
	if (ctx) br_sslio_close(&ctx->ioc);
	
	Mem_Free(ctx_);
	return 0;
}
#elif defined CC_BUILD_3DS
#include <3ds.h>
#include "String.h"
#define CERT_ATTRIBUTES
#include "../misc/RootCerts.h"

// https://github.com/devkitPro/3ds-examples/blob/master/network/sslc/source/ssl.c
// https://github.com/devkitPro/libctru/blob/master/libctru/include/3ds/services/sslc.h
static u32 certChainHandle;
static cc_bool _verifyCerts;

static void SSL_CreateRootChain(void) {
	int ret;

	ret = sslcCreateRootCertChain(&certChainHandle);
	if (ret) { Platform_Log1("sslcCreateRootCertChain failed: %i", &ret); return; }
		
	ret = sslcAddTrustedRootCA(certChainHandle, Baltimore_RootCert, Baltimore_RootCert_Size, NULL);
	if (ret) { Platform_Log1("sslcAddTrustedRootCA failed: %i", &ret); return; }
}

void SSLBackend_Init(cc_bool verifyCerts) {
	int ret = sslcInit(0);
	if (ret) { Platform_Log1("sslcInit failed: %i", &ret); return; }
	
	_verifyCerts = verifyCerts;
	SSL_CreateRootChain();
}
cc_bool SSLBackend_DescribeError(cc_result res, cc_string* dst) { return false; }

cc_result SSL_Init(cc_socket socket, const cc_string* host_, void** out_ctx) {
	if (!certChainHandle) return HTTP_ERR_NO_SSL;
	int ret;
	
	sslcContext* ctx;
	char host[NATIVE_STR_LEN];
	String_EncodeUtf8(host, host_);
	
	ctx = Mem_TryAllocCleared(1, sizeof(sslcContext));
	if (!ctx) return ERR_OUT_OF_MEMORY;
	*out_ctx = (void*)ctx;
	
	int opts = _verifyCerts ? SSLCOPT_Default : SSLCOPT_DisableVerify;
	if ((ret = sslcCreateContext(ctx, socket, opts, host))) return ret;
	Platform_LogConst("--ssl context create--");
	sslcContextSetRootCertChain(ctx, certChainHandle);
	Platform_LogConst("--ssl root chain added--");
	
	// detect lack of proper SSL support in Citra
	if (!ctx->sslchandle) return HTTP_ERR_NO_SSL;
	if ((ret = sslcStartConnection(ctx, NULL, NULL))) return ret;
	Platform_LogConst("--ssl connection started--");
	return 0;
}

cc_result SSL_Read(void* ctx_, cc_uint8* data, cc_uint32 count, cc_uint32* read) { 
	Platform_Log1("<< IN: %i", &count); 
	sslcContext* ctx = (sslcContext*)ctx_;
	int ret = sslcRead(ctx, data, count, false);
	
	Platform_Log1("--ssl read-- = %i", &ret);
	if (ret < 0) return ret;
	*read = ret; return 0;
}

cc_result SSL_Write(void* ctx_, const cc_uint8* data, cc_uint32 count, cc_uint32* wrote) {
	Platform_Log1(">> OUT: %i", &count); 
	sslcContext* ctx = (sslcContext*)ctx_;
	int ret = sslcWrite(ctx, data, count);
	
	Platform_Log1("--ssl write-- = %i", &ret);
	if (ret < 0) return ret;
	*wrote = ret; return 0;
}

cc_result SSL_Free(void* ctx_) { 
	sslcContext* ctx = (sslcContext*)ctx_;
	return sslcDestroyContext(ctx);
}
#elif defined CC_BUILD_GCWII && defined HW_RVL
/* Based off https://wiibrew.org/wiki//dev/net/ssl/code */
#include <gccore.h>
#include "SSL.h"
#include "Platform.h"
#include "Logger.h"
#include "String.h"

#define IOCTLV_SSL_NEW 1
#define IOCTLV_SSL_CONNECT 2
#define IOCTLV_SSL_HANDSHAKE 3
#define IOCTLV_SSL_READ 4
#define IOCTLV_SSL_WRITE 5
#define IOCTLV_SSL_SHUTDOWN 6
#define SSL_HEAP_SIZE 0xB000

#define CERT_ATTRIBUTES ATTRIBUTE_ALIGN(32)
//#include "../misc/RootCerts.h"

static char SSL_fs[] ATTRIBUTE_ALIGN(32) = "/dev/net/ssl";
static s32 SSL_fd  = -1;
static s32 SSL_hid = -1;
void SSLBackend_Init(cc_bool verifyCerts) {
	if (SSL_hid >= 0) return;
	
	SSL_hid = iosCreateHeap(SSL_HEAP_SIZE);
	if (SSL_hid < 0) Logger_Abort("Failed to create SSL heap");
}
cc_bool SSLBackend_DescribeError(cc_result res, cc_string* dst) { return false; }

static u32 ssl_open(void) {
	s32 ret;
	if (SSL_fd >= 0) return 0;
	if (SSL_hid < 0) return ERR_OUT_OF_MEMORY;
	
	ret = IOS_Open(SSL_fs, 0);
	if (ret < 0) return ret;
	SSL_fd = ret;
	return 0;
}

static u32 ssl_close(void) {
	s32 ret;
	if (SSL_fd < 0) return 0;
	
	ret = IOS_Close(SSL_fd);
	SSL_fd = -1;
	return ret;
}

static s32 ssl_new(const cc_string* host, u32 ssl_verify_options) {
	static cc_string ccnet_cert_CN = String_FromConst("sni.cloudflaressl.com");
	s32 ret;
	
	u8 aCN[1024] ATTRIBUTE_ALIGN(32);
	s32 aContext[8] ATTRIBUTE_ALIGN(32);
	u32 aVerify_options[8] ATTRIBUTE_ALIGN(32);
	
	// classicube.net's SSL certificate CN is actually "sni.cloudflaressl.com"
	if (String_CaselessEqualsConst(host, "www.classicube.net")) {
		String_EncodeUtf8(aCN, &ccnet_cert_CN);
	} else {
		String_EncodeUtf8(aCN, host);
	}
	
	if ((ret = ssl_open())) return ret;
	
	aVerify_options[0] = ssl_verify_options;
	ret = IOS_IoctlvFormat(SSL_hid, SSL_fd, IOCTLV_SSL_NEW, "d:dd", aContext, 0x20, aVerify_options, 0x20, aCN, 0x100);
	ssl_close();
	
	return ret ? ret : aContext[0];
}

static s32 ssl_connect(s32 ssl_context, s32 socket) {
	s32 ret;
	s32 aSsl_context[8] ATTRIBUTE_ALIGN(32);
	s32 aSocket[8] ATTRIBUTE_ALIGN(32);
	s32 aResponse[8] ATTRIBUTE_ALIGN(32);
	
	if ((ret = ssl_open())) return ret;
	
	aSsl_context[0] = ssl_context;
	aSocket[0]      = socket;
	ret = IOS_IoctlvFormat(SSL_hid, SSL_fd, IOCTLV_SSL_CONNECT, "d:dd", aResponse, 0x20, aSsl_context, 0x20, aSocket, 0x20);	
	ssl_close();
	
	return ret ? ret : aResponse[0];
}

static s32 ssl_handshake(s32 ssl_context) {
	s32 ret;
	s32 aSsl_context[8] ATTRIBUTE_ALIGN(32);
	s32 aResponse[8] ATTRIBUTE_ALIGN(32);
	
	if ((ret = ssl_open())) return ret;
	
	aSsl_context[0] = ssl_context;
	ret = IOS_IoctlvFormat(SSL_hid, SSL_fd, IOCTLV_SSL_HANDSHAKE, "d:d", aResponse, 0x20, aSsl_context, 0x20);	
	ssl_close();
	
	return ret ? ret : aResponse[0];
}

cc_result SSL_Init(cc_socket socket, const cc_string* host, void** ctx) {
	int sslCtx, ret;
	
	sslCtx = ssl_new(host, 0);
	if (sslCtx < 0) return sslCtx;
	
	int* mem = Mem_Alloc(1, sizeof(int), "SSL context");
	*mem     = sslCtx;
	*ctx     = mem;
	
	if ((ret = ssl_connect(sslCtx, socket))) return ret;
	if ((ret = ssl_handshake(sslCtx)))       return ret;
	return 0;
}

cc_result SSL_Read(void* ctx, cc_uint8* data, cc_uint32 count, cc_uint32* read) {
	int sslCtx = *(int*)ctx;
	*read = 0;
	s32 ret;
	
	s32 aSsl_context[8] ATTRIBUTE_ALIGN(32);
	s32 aResponse[8] ATTRIBUTE_ALIGN(32);
	if ((ret = ssl_open())) return ret;
	
	u8* aBuffer = NULL;
	aBuffer = iosAlloc(SSL_hid, count);
	if (!aBuffer) return IPC_ENOMEM;
	aSsl_context[0] = sslCtx;
	ret = IOS_IoctlvFormat(SSL_hid, SSL_fd, IOCTLV_SSL_READ, "dd:d", aResponse, 0x20, aBuffer, count, aSsl_context, 0x20);
	ssl_close();
	
	if (ret == IPC_OK) {
		Mem_Copy(data, aBuffer, aResponse[0]);
	}
	*read = aResponse[0];
	iosFree(SSL_hid, aBuffer);
	return ret;
}

cc_result SSL_Write(void* ctx, const cc_uint8* data, cc_uint32 count, cc_uint32* wrote) {
	int sslCtx = *(int*)ctx;
	*wrote = 0;
	s32 ret;
	
	s32 aSsl_context[8] ATTRIBUTE_ALIGN(32);
	s32 aResponse[8] ATTRIBUTE_ALIGN(32);
	if ((ret = ssl_open())) return ret;
	
	u8* aBuffer = NULL;
	aBuffer = iosAlloc(SSL_hid, count);
	if (!aBuffer) return IPC_ENOMEM;
	aSsl_context[0] = sslCtx;
	Mem_Copy(aBuffer, data, count);
	ret = IOS_IoctlvFormat(SSL_hid, SSL_fd, IOCTLV_SSL_WRITE, "d:dd", aResponse, 0x20, aSsl_context, 0x20, aBuffer, count);
	ssl_close();
	
	*wrote = aResponse[0];
	iosFree(SSL_hid, aBuffer);
	return ret;
}

cc_result SSL_Free(void* ctx) {
	int sslCtx = *(int*)ctx;
	s32 ret;
	
	s32 aSsl_context[8] ATTRIBUTE_ALIGN(32);
	s32 aResponse[8] ATTRIBUTE_ALIGN(32);	
	if ((ret = ssl_open())) return ret;
	
	aSsl_context[0] = sslCtx;
	ret = IOS_IoctlvFormat(SSL_hid, SSL_fd, IOCTLV_SSL_SHUTDOWN, "d:d", aResponse, 0x20, aSsl_context, 0x20);
	ssl_close();
	
	return ret;
}
#else
void SSLBackend_Init(cc_bool verifyCerts) { }
cc_bool SSLBackend_DescribeError(cc_result res, cc_string* dst) { return false; }

cc_result SSL_Init(cc_socket socket, const cc_string* host, void** ctx) {
	return HTTP_ERR_NO_SSL;
}

cc_result SSL_Read(void* ctx, cc_uint8* data, cc_uint32 count, cc_uint32* read) { 
	return ERR_NOT_SUPPORTED; 
}

cc_result SSL_Write(void* ctx, const cc_uint8* data, cc_uint32 count, cc_uint32* wrote) { 
	return ERR_NOT_SUPPORTED; 
}

cc_result SSL_Free(void* ctx) { return 0; }
#endif