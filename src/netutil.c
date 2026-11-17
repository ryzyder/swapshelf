/*
 * netutil: utilidades de archivos, CRC32 y descargas HTTP(S) de SwapShelf.
 * WinHTTP en Windows, libcurl en Switch y curl en Linux (pruebas).
 */
#include "netutil.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#elif !defined(__SWITCH__)
#include <unistd.h>
#endif

unsigned char* swsReadFile(const char* path, size_t* len) {
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0) { fclose(f); return NULL; }
	unsigned char* buf = malloc((size_t) n + 1);
	if (!buf) { fclose(f); return NULL; }
	size_t got = fread(buf, 1, (size_t) n, f);
	fclose(f);
	if (got != (size_t) n) { free(buf); return NULL; }
	buf[n] = 0;
	*len = (size_t) n;
	return buf;
}

/* ---- CRC32 (el de No-Intro / zlib) ---- */
static uint32_t crcTable[256];
static void crcInit(void) {
	for (uint32_t i = 0; i < 256; ++i) {
		uint32_t c = i;
		for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crcTable[i] = c;
	}
}
static uint32_t crc32Update(uint32_t crc, const unsigned char* p, size_t n) {
	crc = ~crc;
	while (n--) crc = crcTable[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

void swsInit(void) { crcInit(); }
uint32_t swsCrc32(const unsigned char* p, size_t n) { return crc32Update(0, p, n); }

/* ---- descargas ---- */
#ifdef _WIN32
unsigned char* swsHttpGet(const char* url, size_t* outLen, const char** err,
                         volatile long long* progressDone, volatile long long* progressTotal) {
	wchar_t wurl[2048], host[256], path[2048];
	MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048);
	URL_COMPONENTS uc;
	memset(&uc, 0, sizeof(uc));
	uc.dwStructSize = sizeof(uc);
	uc.lpszHostName = host; uc.dwHostNameLength = 256;
	wchar_t extra[2048];
	uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
	uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
	if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) { *err = "direcci\xC3\xB3n inv\xC3\xA1lida"; return NULL; }
	/* la parte "?a=b" (consultas a la API) va junto a la ruta */
	if (uc.dwExtraInfoLength && wcslen(path) + wcslen(extra) < 2048) wcscat(path, extra);

	HINTERNET s = WinHttpOpen(L"SwapShelf/0.4 (Windows)", 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY */, NULL, NULL, 0);
	if (!s) s = WinHttpOpen(L"SwapShelf/0.4 (Windows)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
	if (!s) { *err = "no se pudo iniciar la conexi\xC3\xB3n"; return NULL; }
	DWORD protocols = 0x00000800 /* TLS 1.2 */ | 0x00002000 /* TLS 1.3 */;
	WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
	WinHttpSetTimeouts(s, 15000, 15000, 30000, 60000);

	unsigned char* buf = NULL;
	HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
	HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
	                                     uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
	if (!r || !WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
	    !WinHttpReceiveResponse(r, NULL)) {
		*err = "no se pudo conectar (revisa tu internet)";
		goto done;
	}
	DWORD code = 0, sz = sizeof(code);
	WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
	if (code != 200) {
		static char msg[64];
		snprintf(msg, sizeof(msg), "el servidor respondi\xC3\xB3 %lu", (unsigned long) code);
		*err = msg;
		goto done;
	}
	DWORD total = 0;
	sz = sizeof(total);
	if (!WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &total, &sz, WINHTTP_NO_HEADER_INDEX)) total = 0;
	size_t cap = total ? total + 1 : 1 << 20, len = 0;
	buf = malloc(cap);
	if (progressTotal) *progressTotal = total;
	if (progressDone) *progressDone = 0;
	for (;;) {
		DWORD avail = 0, got = 0;
		if (!WinHttpQueryDataAvailable(r, &avail)) { free(buf); buf = NULL; *err = "la descarga se cort\xC3\xB3"; goto done; }
		if (!avail) break;
		if (len + avail + 1 > cap) {
			while (len + avail + 1 > cap) cap *= 2;
			buf = realloc(buf, cap);
		}
		if (!WinHttpReadData(r, buf + len, avail, &got)) { free(buf); buf = NULL; *err = "la descarga se cort\xC3\xB3"; goto done; }
		len += got;
		if (progressDone) *progressDone = (long long) len;
	}
	buf[len] = 0;
	*outLen = len;
done:
	if (r) WinHttpCloseHandle(r);
	if (c) WinHttpCloseHandle(c);
	WinHttpCloseHandle(s);
	return buf;
}
#elif defined(__SWITCH__)
/* Switch: libcurl de devkitPro (switch-curl). La red se inicia con socketInitializeDefault(). */
#include <curl/curl.h>

struct SwsBuf { unsigned char* data; size_t len, cap; volatile long long* done; };

static size_t swsCurlWrite(void* ptr, size_t size, size_t nmemb, void* user) {
	struct SwsBuf* b = user;
	size_t n = size * nmemb;
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 1 << 20;
		while (b->len + n + 1 > cap) cap *= 2;
		unsigned char* nd = realloc(b->data, cap);
		if (!nd) return 0;
		b->data = nd;
		b->cap = cap;
	}
	memcpy(b->data + b->len, ptr, n);
	b->len += n;
	if (b->done) *b->done = (long long) b->len;
	return n;
}

static int swsCurlProgress(void* user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
	(void) dlnow; (void) ultotal; (void) ulnow;
	volatile long long* total = user;
	if (total && dltotal > 0) *total = (long long) dltotal;
	return 0;
}

unsigned char* swsHttpGet(const char* url, size_t* outLen, const char** err,
                         volatile long long* progressDone, volatile long long* progressTotal) {
	static bool inited = false;
	if (!inited) { curl_global_init(CURL_GLOBAL_DEFAULT); inited = true; }
	CURL* c = curl_easy_init();
	if (!c) { *err = "no se pudo iniciar la red"; return NULL; }
	struct SwsBuf b = { NULL, 0, 0, progressDone };
	if (progressTotal) *progressTotal = 0;
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "SwapShelf/0.4 (Switch)");
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, swsCurlWrite);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, swsCurlProgress);
	curl_easy_setopt(c, CURLOPT_XFERINFODATA, (void*) progressTotal);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
	CURLcode rc = curl_easy_perform(c);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	if (rc != CURLE_OK || code != 200) {
		free(b.data);
		static char msg[96];
		if (rc != CURLE_OK) snprintf(msg, sizeof(msg), "sin conexi\xC3\xB3n (%s)", curl_easy_strerror(rc));
		else snprintf(msg, sizeof(msg), "el servidor respondi\xC3\xB3 %ld", code);
		*err = msg;
		return NULL;
	}
	if (!b.data) { b.data = malloc(1); }
	b.data[b.len] = 0;
	*outLen = b.len;
	return b.data;
}
#else
/* Linux: curl (o, para pruebas, SWS_TEST_HTTP con los .bps ya descargados) */
unsigned char* swsHttpGet(const char* url, size_t* outLen, const char** err,
                         volatile long long* progressDone, volatile long long* progressTotal) {
	const char* testDir = getenv("SWS_TEST_HTTP");
	if (testDir) {
		char p[1024];
		snprintf(p, sizeof(p), "%s/%s", testDir, strrchr(url, '/') + 1);
		unsigned char* b = swsReadFile(p, outLen);
		if (!b) { *err = "no se pudo descargar (prueba)"; return NULL; }
		if (progressTotal) *progressTotal = (long long) *outLen;
		if (progressDone) *progressDone = (long long) *outLen;
		return b;
	}
	char tmp[] = "/tmp/swapshelf-dl-XXXXXX";
	int fd = mkstemp(tmp);
	if (fd < 0) { *err = "sin archivo temporal"; return NULL; }
	close(fd);
	char cmd[2400];
	snprintf(cmd, sizeof(cmd), "curl -sSL --fail -o '%s' '%s' 2>/dev/null", tmp, url);
	if (system(cmd) != 0) { remove(tmp); *err = "no se pudo descargar"; return NULL; }
	unsigned char* b = swsReadFile(tmp, outLen);
	remove(tmp);
	if (!b) *err = "no se pudo leer la descarga";
	else { if (progressTotal) *progressTotal = (long long) *outLen; if (progressDone) *progressDone = (long long) *outLen; }
	return b;
}
#endif

