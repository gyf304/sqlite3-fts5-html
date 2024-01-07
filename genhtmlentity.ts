// @ts-ignore
import * as fs from "node:fs/promises";

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

const parts: string[] = [];

parts.push(`
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
	parts.push(`\t{"${name}", "${escaped}"},`);
}

parts.push(`
	{0, 0}
};

#define MAX_ENTITY_NAME_LENGTH ${Math.max(...Object.keys(cleaned).map((k) => k.length))}
#define NUM_ENTITIES ${Object.keys(cleaned).length}
`);


const input = await fs.readFile("fts5html.c", "utf-8");
let emit = true;
const outputParts: string[] = [];

for (let line of input.split("\n")) {
	if (line.includes("/* START OF HTML ENTITIES */")) {
		outputParts.push(line);
		emit = false;
	}
	if (emit) {
		outputParts.push(line);
	}
	if (line.includes("/* END OF HTML ENTITIES */")) {
		emit = true;
		outputParts.push(parts.join("\n"));
		outputParts.push(line);
	}
}

await fs.writeFile("fts5html.c", outputParts.join("\n"));
