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

#include <string.h>

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1

#ifdef SQLITE_OMIT_LOAD_EXTENSION
/* note: if you are on macOS - do not use included SQLite sqlite3ext.h */
#error "The sqlite3ext.h header defines SQLITE_OMIT_LOAD_EXTENSION"
#endif

static const char *azIgnoreTags[] = {
	"base",
	"canvas",
	"embed",
	"link",
	"math",
	"meta",
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
	int offset;
};
typedef struct Fts5HtmlTokenizerContext Fts5HtmlTokenizerContext;

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

static inline int isWhitespace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static inline char lower(char c) {
	if (c >= 'A' && c <= 'Z') {
		return c - 'A' + 'a';
	}
	return c;
}

static inline int caseInsensitiveCompare(const char *a, const char *b, int n) {
	for (int i = 0; i < n; i++) {
		char ca = lower(a[i]);
		char cb = lower(b[i]);
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
	return caseInsensitiveCompare(p, prefix, len) == 0;
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
	return p->xToken(p->pCtx, tflags, pToken, nToken, p->offset + iStart, p->offset + iEnd);
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
			Fts5HtmlTokenizerContext ctx = {
				.xToken = xToken,
				.pCtx = pCtx,
				.offset = pPrev - pText,
			};
			int len = pCur - pPrev;
			/* emit the text token */
			rc = p->nextTok.xTokenize(p->pNextTokInst, &ctx, flags, pPrev, len, fts5TokenizeCallback);
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
		while (!isWhitespace(*pCur) && *pCur != '>') {
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
				if (hasPrefix(pTagName, nTagName, azIgnoreTags[i])) {
					pzCurIgnoreTag = azIgnoreTags[i];
					break;
				}
			}
		} else if (pzCurIgnoreTag != NULL && iTagType == 2) {
			if (hasPrefix(pTagName, nTagName, pzCurIgnoreTag)) {
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
	int rc = SQLITE_OK;
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
