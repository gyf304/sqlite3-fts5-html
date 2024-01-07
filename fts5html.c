/*
** 2023-12-25
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*/
#include <ctype.h>
#include <string.h>

#ifdef SQLITE_OMIT_LOAD_EXTENSION
#define SQLITE_OMIT_LOAD_EXTENSION_PREINCLUDE
#endif

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1

#if defined(SQLITE_OMIT_LOAD_EXTENSION) && !defined(SQLITE_OMIT_LOAD_EXTENSION_PREINCLUDE)
/* note: if you are on macOS - do not use included SQLite sqlite3ext.h */
#error "The sqlite3ext.h header defines SQLITE_OMIT_LOAD_EXTENSION"
#endif

#include "htmlentity.h"

/* do not include void element here */
static const char *azIgnoreTags[] = {
	"canvas",
	"math",
	"noscript",
	"object",
	"script",
	"style",
	"svg",
	"template",
	NULL,
};

struct Fts5HtmlTokenizer {
	fts5_tokenizer nextTok;
	Fts5Tokenizer *pNextTokInst;
};
typedef struct Fts5HtmlTokenizer Fts5HtmlTokenizer;

struct htmlEscape {
	char *pLengths;
	char *pPlain;
	int n;
};
typedef struct htmlEscape htmlEscape;

struct Fts5HtmlTokenizerContext {
	int (*xToken)(
		void *pCtx,
		int tflags,
		const char *pToken,
		int nToken,
		int iStart,
		int iEnd
	);
	void *pCtx;
	htmlEscape *pEscape;

	/* current position in the plain (unescaped) text, relative to the what is
	 * passed to the xTokenize call to the next tokenizer
	 */
	int iPlainCur;

	/* current position in the original (html) text
	 * this is relative to the original text passed to the xTokenize call to
	 * this tokenizer
	 */
	int iOriginalCur;
};
typedef struct Fts5HtmlTokenizerContext Fts5HtmlTokenizerContext;

static const htmlEntity *findEntity(const char *s, int len) {
	int l = 0;
	int r = NUM_ENTITIES - 1;
	while (l <= r) {
		int m = (l + r) / 2;
		int cmp = strncmp(s, htmlEntities[m].pzName, len);
		if (cmp == 0) {
			return &htmlEntities[m];
		} else if (cmp < 0) {
			r = m - 1;
		} else {
			l = m + 1;
		}
	}

	return NULL;
}

static inline int caseInsensitiveCompare(const char *a, const char *b, int n) {
	for (int i = 0; i < n; i++) {
		char ca = (char)tolower(a[i]);
		char cb = (char)tolower(b[i]);
		int diff = ca - cb;
		if (diff != 0 || ca == '\0' || cb == '\0') {
			return diff;
		}
	}
	return 0;
}

static inline int hasPrefix(const char *p, int n, const char *prefix) {
	int len = strlen(prefix);
	if (n < len) {
		return 0;
	}
	return strncmp(p, prefix, len) == 0;
}

static int fts5HtmlTokenizerCreate(void *pCtx, const char **azArg, int nArg, Fts5Tokenizer **ppOut) {
	int rc = SQLITE_OK;
	fts5_api *pApi = (fts5_api*)pCtx;
	Fts5HtmlTokenizer *pRet = NULL;
	void *pTokCtx = NULL;

	if (nArg == 0) {
		return SQLITE_MISUSE;
	}

	pRet = sqlite3_malloc(sizeof(*pRet));
	if (pRet == NULL) {
		rc = SQLITE_NOMEM;
		goto error;
	}
	memset(pRet, 0, sizeof(*pRet));

	const char *nextTokName = azArg[0];
	rc = pApi->xFindTokenizer(pApi, nextTokName, &pTokCtx, &pRet->nextTok);
	if (rc != SQLITE_OK) {
		goto error;
	}

	rc = pRet->nextTok.xCreate(pTokCtx, azArg + 1, nArg - 1, &pRet->pNextTokInst);
	if (rc != SQLITE_OK) {
		goto error;
	}

	*ppOut = (Fts5Tokenizer*)pRet;
	return SQLITE_OK;

error:
	if (pRet != NULL) {
		sqlite3_free(pRet);
	}
	return rc;
}

static int fts5TokenizeCallback(
	void *pCtx,
	int tflags,
	const char *pToken,
	int nToken,
	int iStart,
	int iEnd
) {
	Fts5HtmlTokenizerContext *p = (Fts5HtmlTokenizerContext*)pCtx;
	htmlEscape *e = p->pEscape;

	int iActualStart = p->iOriginalCur;
	if (p->iPlainCur > iStart) {
		return SQLITE_ERROR;
	}

	for (int i = p->iPlainCur; i < iStart; i++) {
		iActualStart += e->pLengths[i];
	}
	int iActualEnd = iActualStart;
	for (int i = iStart; i < iEnd; i++) {
		iActualEnd += e->pLengths[i];
	}
	p->iOriginalCur = iActualEnd;
	p->iPlainCur = iEnd;

	return p->xToken(p->pCtx, tflags, pToken, nToken, iActualStart, iActualEnd);
}

static int parseCodepoint(const char *s, int len) {
	int code = 0;
	if (len < 1) {
		return -1;
	}
	if (s[0] == 'x' || s[0] == 'X') {
		/* hex, e.g. &#x123; */
		for (int i = 1; i < len; i++) {
			char d = s[i];
			code *= 16;
			if (d >= '0' && d <= '9') {
				code += d - '0';
			} else if (d >= 'a' && d <= 'f') {
				code += d - 'a' + 10;
			} else if (d >= 'A' && d <= 'F') {
				code += d - 'A' + 10;
			} else {
				return -1;
			}
		}
	} else {
		/* decimal, e.g. &#123; */
		for (int i = 0; i < len; i++) {
			char d = s[i];
			code *= 10;
			if (d >= '0' && d <= '9') {
				code += d - '0';
			} else {
				return -1;
			}
		}
	}
	return code;
}

static void htmlEscapeFree(htmlEscape *p) {
	if (p != NULL) {
		if (p->pLengths != NULL) {
			sqlite3_free(p->pLengths);
		}
		if (p->pPlain != NULL) {
			sqlite3_free(p->pPlain);
		}
		sqlite3_free(p);
	}
}

static int htmlUnescape(const char *s, int len, htmlEscape **pOutEscape) {
	if (len <= 0) {
		*pOutEscape = NULL;
		return SQLITE_OK;
	}
	int bufLen = len + 16;

	int rc = SQLITE_OK;
	htmlEscape *e = sqlite3_malloc(sizeof(*e));
	if (e == NULL) {
		rc = SQLITE_NOMEM;
		goto end;
	}
	memset(e, 0, sizeof(*e));

	e->pLengths = sqlite3_malloc(bufLen);
	if (e->pLengths == NULL) {
		rc = SQLITE_NOMEM;
		goto end;
	}
	memset(e->pLengths, 0, bufLen);

	e->pPlain = sqlite3_malloc(bufLen);
	if (e->pPlain == NULL) {
		rc = SQLITE_NOMEM;
		goto end;
	}
	memset(e->pPlain, 0, bufLen);

	const char *p = s;
	const char *pEmit = s;

	char *ep = e->pPlain;
	char *el = e->pLengths;

#define EMIT(c) do {\
	if (ep >= e->pPlain + bufLen) {\
		rc = SQLITE_ERROR;\
		goto end;\
	}\
	*ep++ = c;\
	*el++ = (p + 1) - pEmit;\
	pEmit = p + 1;\
} while (0)

	unsigned long escaped = 0;
	char buf[MAX_ENTITY_NAME_LENGTH + 4] = {0};

	for (; p - s < len; p++) {
		char c = *p;
		if (escaped > 0) {
			if (!isalnum(c) && c != '#') {
				if (buf[1] == '#') {
					/* numeric escape, buf: &#0000 or &#x0000 */
					int code = parseCodepoint(buf + 2, escaped - 2);
					/* assume code is a Unicode code point, encode into UTF-8 */
					if (code < 0) {
						/* invalid escape, */
					} else if (code < 0x80) {
						EMIT(code);
					} else if (code < 0x800) {
						EMIT(0xC0 | (code >> 6));
						EMIT(0x80 | (code & 0x3F));
					} else if (code < 0x10000) {
						EMIT(0xE0 | (code >> 12));
						EMIT(0x80 | ((code >> 6) & 0x3F));
						EMIT(0x80 | (code & 0x3F));
					} else if (code < 0x110000) {
						EMIT(0xF0 | (code >> 18));
						EMIT(0x80 | ((code >> 12) & 0x3F));
						EMIT(0x80 | ((code >> 6) & 0x3F));
						EMIT(0x80 | (code & 0x3F));
					} else {
						/* invalid escape, ignore */
					}
				} else {
					/* named escape, buf: &amp; */
					const htmlEntity *entity = findEntity(buf + 1, escaped - 1);
					if (entity != NULL) {
						const char *pEntity = entity->pzUtf8;
						while (*pEntity != '\0') {
							EMIT(*pEntity);
							pEntity++;
						}
					} else {
						/* invalid escape, ignore */
					}
				}
				if (c != ';') {
					/* unclosed entity */
					EMIT(c);
				}
				escaped = 0;
			} else {
				if (escaped < sizeof(buf)) {
					buf[escaped++] = c;
				}
			}
		} else {
			/* not escaped */
			if (c == '&') {
				escaped = 0;
				memset(buf, 0, sizeof(buf));
				buf[escaped++] = c;
			} else {
				EMIT(c);
			}
		}
	}

	e->n = ep - e->pPlain;

	*pOutEscape = e;
	return rc;

#undef EMIT
end:
	htmlEscapeFree(e);

	*pOutEscape = e;
	return rc;
}

static int fts5HtmlTokenizerTokenize(
	Fts5Tokenizer *pTokenizer,
	void *pCtx,
	int flags,
	const char *pText,
	int nText,
	int (*xToken)(
		void *pCtx,
		int tflags,
		const char *pToken,
		int nToken,
		int iStart,
		int iEnd
	)
) {
	int rc;
	Fts5HtmlTokenizer *p = (Fts5HtmlTokenizer*)pTokenizer;

	// states
	const char *pzCurIgnoreTag = NULL;

	// iterate over the tokens
	const char *pPrev = pText;
	const char *pCur = pText;
	const char *pEnd = pText + nText;

	while (1) {

#define STEP(n) do {\
	if (pCur + n >= pEnd) { \
		return SQLITE_OK; \
	} \
	pCur += n; \
} while (0)

		/* find the next tag */
		while (*pCur != '<' && pCur < pEnd) {
			pCur++;
		}

		/* current is '<' or end of text */
		if (pzCurIgnoreTag == NULL && pCur > pPrev) {
			htmlEscape *pEscape = NULL;
			rc = htmlUnescape(pPrev, pCur - pPrev, &pEscape);
			if (rc != SQLITE_OK) {
				return rc;
			}
			Fts5HtmlTokenizerContext ctx = {
				.xToken = xToken,
				.pCtx = pCtx,
				.pEscape = pEscape,
				.iPlainCur = 0,
				.iOriginalCur = pPrev - pText,
			};
			/* emit the text token */
			rc = p->nextTok.xTokenize(p->pNextTokInst, &ctx, flags, pEscape->pPlain, pEscape->n, fts5TokenizeCallback);
			htmlEscapeFree(pEscape);
			if (rc != SQLITE_OK) {
				return rc;
			}
		}
		STEP(1);

		/* check for comment */
		if (hasPrefix(pCur, pEnd - pCur, "!--")) {
			STEP(3);
			while (!hasPrefix(pCur, pEnd - pCur, "-->")) {
				STEP(1);
			}
			STEP(3);
			continue;
		}

		/* parse the tag */
		int iTagType = 0; /* 0: self-closing, 1: start tag, 2: end tag */
		const char *pTagName;
		int nTagName;
		if (*pCur == '/') { /* </... */
			iTagType = 2;
			STEP(1);
		} else {
			iTagType = 1;
		}
		pTagName = pCur;
		while (!isspace(*pCur) && *pCur != '>') {
			STEP(1);
		}
		nTagName = pCur - pTagName;
		while (*pCur != '>') {
			STEP(1);
		}
		if (*(pCur - 1) == '/') {
			iTagType = 0;
		}

		/* check if tag is ignored */
		if (pzCurIgnoreTag == NULL && iTagType == 1) {
			for (int i = 0; azIgnoreTags[i] != NULL; i++) {
				if (caseInsensitiveCompare(pTagName, azIgnoreTags[i], nTagName) == 0) {
					pzCurIgnoreTag = azIgnoreTags[i];
					break;
				}
			}
		} else if (pzCurIgnoreTag != NULL && iTagType == 2) {
			if (caseInsensitiveCompare(pTagName, pzCurIgnoreTag, nTagName) == 0) {
				pzCurIgnoreTag = NULL;
			}
		}

		/* tag ended */
		STEP(1);
		pPrev = pCur;

#undef STEP

	}

	return SQLITE_OK;
}

static void fts5HtmlTokenizerDelete(Fts5Tokenizer *pTokenizer) {
	Fts5HtmlTokenizer *p = (Fts5HtmlTokenizer*)pTokenizer;
	p->nextTok.xDelete(p->pNextTokInst);
	sqlite3_free(p);
}

static int fts5ApiFromDb(sqlite3 *db, fts5_api **ppApi){
	fts5_api *pRet = NULL;
	sqlite3_stmt *pStmt = NULL;
	int rc = SQLITE_OK;

	rc = sqlite3_prepare(db, "SELECT fts5(?1)", -1, &pStmt, NULL);
	if (rc != SQLITE_OK) {
		return rc;
	}

	rc = sqlite3_bind_pointer(pStmt, 1, (void*)&pRet, "fts5_api_ptr", NULL);
	if (rc != SQLITE_OK) {
		goto finalize;
	}

	rc = sqlite3_step(pStmt);
	if (rc != SQLITE_ROW) {
		goto finalize;
	}
	rc = SQLITE_OK;

finalize:
	sqlite3_finalize(pStmt);
	*ppApi = pRet;
	return rc;
}

static int fts5HtmlInit(sqlite3 *db) {
	fts5_api *pApi = NULL;
	int rc = SQLITE_OK;
	rc = fts5ApiFromDb(db, &pApi);
	if (rc != SQLITE_OK) {
		return rc;
	}

	fts5_tokenizer tok = {
		.xCreate = fts5HtmlTokenizerCreate,
		.xTokenize = fts5HtmlTokenizerTokenize,
		.xDelete = fts5HtmlTokenizerDelete,
	};

	return pApi->xCreateTokenizer(pApi, "html", (void *)pApi, &tok, NULL);
}

#ifndef SQLITE_CORE
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_ftshtml_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
	SQLITE_EXTENSION_INIT2(pApi);
	(void)pzErrMsg; /* unused */
	return fts5HtmlInit(db);
}
#else
int sqlite3Fts5HtmlInit(sqlite3 *db) {
	return fts5HtmlInit(db);
}
#endif
