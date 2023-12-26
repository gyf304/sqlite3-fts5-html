# HTML Tokenizer for SQLite3 FTS5

This is a pseudo tokenizer that can be used with SQLite3's FTS5
extension to index HTML documents. It understands just enough HTML to
extract the text from the document and ignore the markup.

## Building

Ensure that you have the header `sqlite3ext.h` in your include path.

```sh
make
```

## Usage

```sql
.load ./fts5html.so -- change .so to .dll / .dylib as appropriate

CREATE VIRTUAL TABLE docs USING fts5(
  content,
  -- html itself is not a tokenizer
  -- it must be must be used with another tokenizer
  tokenize = 'html unicode61 remove_diacritics 1'
);
```
