import { expect, test } from "vitest";
import { findFirmwareDevice } from "./catalog.ts";
import {
  discoverMind,
  finishMindSignIn,
  platformOriginCandidates,
  startMindSignIn,
} from "./mind-sign-in.ts";

const device = findFirmwareDevice("home-assistant-voice-preview-edition")!;

function memoryStorage(): Storage {
  const items = new Map<string, string>();
  return {
    get length() {
      return items.size;
    },
    clear: () => items.clear(),
    getItem: (key) => items.get(key) ?? null,
    key: (index) => [...items.keys()][index] ?? null,
    removeItem: (key) => void items.delete(key),
    setItem: (key, value) => void items.set(key, value),
  };
}

const metadata = (origin: string) => ({
  issuer: origin,
  authorization_endpoint: `${origin}/oauth2/auth`,
  token_endpoint: `${origin}/oauth2/token`,
  protected_resources: [`${origin}/api`, `${origin}/mcp`],
});

test("platformOriginCandidates: a domain names its platform host, and a platform host names itself too", () => {
  expect(platformOriginCandidates("templestein.family")).toEqual([
    "https://iterate.templestein.family",
  ]);
  expect(platformOriginCandidates(" https://iterate.iterate.com/ ")).toEqual([
    "https://iterate.iterate.iterate.com",
    "https://iterate.iterate.com",
  ]);
  expect(() => platformOriginCandidates("localhost")).toThrow("whole address");
});

test("discoverMind: takes the first discovery document that names itself and its /api", async () => {
  const asked: string[] = [];
  const found = await discoverMind("iterate.iterate.com", (async (url: string) => {
    asked.push(url);
    if (url.startsWith("https://iterate.iterate.com/"))
      return Response.json(metadata("https://iterate.iterate.com"));
    return new Response("no", { status: 404 });
  }) as typeof fetch);
  expect(found.platformOrigin).toBe("https://iterate.iterate.com");
  expect(asked).toHaveLength(2);
  await expect(
    discoverMind("example.com", (async () =>
      Response.json({
        ...metadata("https://iterate.example.com"),
        issuer: "https://elsewhere.com",
      })) as typeof fetch),
  ).rejects.toThrow("no iterate mind");
});

test("a sign-in: the authorize URL carries PKCE and the board's own client, and the code becomes the board's refresh token", async () => {
  const storage = memoryStorage();
  const origin = "https://iterate.example.com";
  const authorize = new URL(
    await startMindSignIn({
      device,
      address: "example.com",
      kitOrigin: "https://k.iterate.com",
      storage,
      fetcher: (async () => Response.json(metadata(origin))) as typeof fetch,
    }),
  );
  expect(authorize.origin + authorize.pathname).toBe(`${origin}/oauth2/auth`);
  const params = authorize.searchParams;
  expect(params.get("client_id")).toMatch(
    /^https:\/\/k\.iterate\.com\/devices\/home-assistant-voice-preview-edition\/clients\/[0-9a-f-]{36}\.json$/,
  );
  expect(params.get("redirect_uri")).toBe(
    "https://k.iterate.com/devices/home-assistant-voice-preview-edition/mind",
  );
  expect(params.get("resource")).toBe(`${origin}/api`);
  expect(params.get("code_challenge_method")).toBe("S256");

  let posted: URLSearchParams | undefined;
  const signIn = await finishMindSignIn({
    device,
    code: "the-code",
    state: params.get("state")!,
    storage,
    fetcher: (async (_url: string, init: RequestInit) => {
      posted = init.body as URLSearchParams;
      return Response.json({
        access_token: "a",
        refresh_token: "user_1:grant_1:r",
        expires_in: 3600,
      });
    }) as typeof fetch,
  });
  expect(signIn).toEqual({
    address: "example.com",
    platformOrigin: origin,
    clientId: params.get("client_id"),
    refreshToken: "user_1:grant_1:r",
  });
  expect(posted?.get("grant_type")).toBe("authorization_code");
  expect(posted?.get("code_verifier")).toMatch(/^[A-Za-z0-9_-]{43}$/);
  // used once: a replayed redirect finds nothing to finish
  await expect(
    finishMindSignIn({
      device,
      code: "the-code",
      state: params.get("state")!,
      storage,
      fetcher: fetch,
    }),
  ).rejects.toThrow("did not start on this page");
});
