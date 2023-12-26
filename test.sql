CREATE VIRTUAL TABLE test USING fts5(x, y, tokenize = 'html unicode61 remove_diacritics 1');

INSERT INTO test VALUES('a', '
<html>
<head>
	<script>ignored</script></head>
<body>
	hello <style>also ignored</style> world</body>
</html>
');

INSERT INTO test VALUES('b', '
<!doctype html>
<html>
<head>
    <title>Example Domain</title>

    <meta charset="utf-8" />
    <meta http-equiv="Content-type" content="text/html; charset=utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <style type="text/css">
    body {
        background-color: #f0f0f2;
        margin: 0;
        padding: 0;
        font-family: -apple-system, system-ui, BlinkMacSystemFont, "Segoe UI", "Open Sans", "Helvetica Neue", Helvetica, Arial, sans-serif;

    }
    div {
        width: 600px;
        margin: 5em auto;
        padding: 2em;
        background-color: #fdfdff;
        border-radius: 0.5em;
        box-shadow: 2px 3px 7px 2px rgba(0,0,0,0.02);
    }
    a:link, a:visited {
        color: #38488f;
        text-decoration: none;
    }
    @media (max-width: 700px) {
        div {
            margin: 0 auto;
            width: auto;
        }
    }
    </style>
</head>

<body>
<div>
    <h1>Example Domain</h1>
    <p>This domain is for use in illustrative examples in documents. You may use this
    domain in literature without prior coordination or asking for permission.</p>
    <p><a href="https://www.iana.org/domains/example">More information...</a></p>
</div>
</body>
</html>
');

SELECT count(*) FROM test WHERE test MATCH 'ignored';     -- expected: 0
SELECT count(*) FROM test WHERE test MATCH 'hello';       -- expected: 1
SELECT count(*) FROM test WHERE test MATCH 'hello world'; -- expected: 1
SELECT count(*) FROM test WHERE test MATCH 'example';     -- expected: 1
SELECT count(*) FROM test WHERE test MATCH 'domain';      -- expected: 1
SELECT count(*) FROM test WHERE test MATCH 'information'; -- expected: 1
SELECT count(*) FROM test WHERE test MATCH 'meta';        -- expected: 0
SELECT count(*) FROM test WHERE test MATCH 'margin';      -- expected: 0
SELECT count(*) FROM test WHERE test MATCH 'viewport';    -- expected: 0
select count(*) from test where test match 'Helvetica';   -- expected: 0
