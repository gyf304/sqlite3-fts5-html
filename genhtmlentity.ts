export {};

interface EntityDefinition {
	characters: string;
}

const entities: Record<string, EntityDefinition> =
	await fetch("https://html.spec.whatwg.org/entities.json").then(r => r.json());

const cleaned = Object.fromEntries(
	Object.entries(entities)
		.map(([k, v]) => [k.replace(/^&(.*?);?$/g, "$1"), v])
);


function strcmp(a: string, b: string): number {
	if (a < b) {
		return -1;
	} else if (a > b) {
		return 1;
	} else {
		return 0;
	}
}

const encoder = new TextEncoder();

console.log(`
#ifndef _HTML_ESCAPE_H
#define _HTML_ESCAPE_H

struct htmlEntity {
	const char *pzName;
	const char *pzUtf8;
};

typedef struct htmlEntity htmlEntity;

static const htmlEntity htmlEntities[] = {
`.trim());

for (const [name, { characters }] of Object.entries(cleaned).sort((a, b) => strcmp(a[0], b[0]))) {
	const hex = encoder.encode(characters);
	const escaped = Array.from(hex).map((h) => "\\x" + h.toString(16).padStart(2, "0")).join("");
	console.log(`\t{"${name}", "${escaped}"},`);
}

console.log(`
	{0, 0}
};

#define MAX_ENTITY_NAME_LENGTH ${Math.max(...Object.keys(cleaned).map((k) => k.length))}
#define NUM_ENTITIES ${Object.keys(cleaned).length}

#endif
`);
