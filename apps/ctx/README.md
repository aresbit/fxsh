# ctx - Thompson NFA regex demo (fxsh)

This project is now implemented in pure `fxsh`, with the reusable engine in `stdlib/regex.fxsh`.
Algorithm follows the Thompson NFA approach popularized by Russ Cox:
https://swtch.com/~rsc/regexp/

## Supported syntax

- Literal characters: `abc`
- Concatenation: implicit (`ab`)
- Alternation: `a|b`
- Kleene star: `a*`
- One or more: `a+`
- Zero or one: `a?`
- Grouping: `( ... )`
- Dot wildcard: `.`
- Escape: `\\` before a metacharacter

## Run

```sh
apps/ctx/bin/ctx --dump 'a(b|c)*d' ad abcd accd axd
```

Output format for match tests:

- `MATCH\t<text>`
- `MISS\t<text>`
