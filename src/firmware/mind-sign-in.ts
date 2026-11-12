/**
 * A BOARD'S SIGN-IN TO A MIND (iterate/core), done in the browser alone. A mind issues no token
 * without an expiry, and its `/api` answers no other origin, so Kit asks the mind's issuer for an
 * ordinary OAuth grant (authorization code with PKCE) for this one board, as its own client, and
 * writes the grant's refresh token into the board. The board trades it for an access token before
 * each connection (firmware/components/core/include/iterate/kit/token_refresh.h).
 *
 * The person signs in at the mind and approves the board there. The tokens come back to this page
 * from the issuer's token endpoint (which answers Kit's origin), live only in this page's memory,
 * and leave it only to the board. Kit's Worker never sees them.
 *
 * The grant lives while the board uses it, and ends after 7 days with no use (the mind's issuer,
 * core/auth-worker `SESSION_IDLE_SECONDS`), when its person stops being a member, or when it is
 * revoked.
 */
import type { FirmwareDevice } from "./catalog.ts";

/** A mind keeps every hostname of its domain but this one (iterate/core `mind-api`). */
const PLATFORM_LABEL = "iterate";

/** What the board needs: written to its image as `MindCredential` (config-image.ts). */
export interface MindSignIn {
  /** The mind's address as the person typed it, e.g. `templestein.family`. */
  address: string;
  /** `https://iterate.<domain>`: the issuer, and `/api` on it the board's resource. */
  platformOrigin: string;
  clientId: string;
  refreshToken: string;
}

/** What survives the trip to the issuer and back, in sessionStorage. */
interface PendingSignIn {
  state: string;
  verifier: string;
  address: string;
  platformOrigin: string;
  tokenEndpoint: string;
  clientId: string;
  redirectUri: string;
}

const pendingKey = (deviceId: string) => `kit:mind-sign-in:${deviceId}`;

/** The hosts a typed address may name, in the order to ask (iterate/core `mindAddressCandidatesOf`):
 *  the platform hostname of the address as a domain, then the address itself when it already is
 *  one (`iterate.example.com` names `example.com`). */
export function platformOriginCandidates(typed: string): string[] {
  const trimmed = typed.trim().toLowerCase();
  if (!trimmed) throw new Error("Enter the mind's address, like example.com.");
  let host: string;
  try {
    host = new URL(/^[a-z][a-z0-9+.-]*:\/\//.test(trimmed) ? trimmed : `https://${trimmed}`)
      .hostname;
  } catch {
    throw new Error("Enter the mind's address, like example.com.");
  }
  host = host.replace(/\.$/, "");
  if (!host.includes(".")) throw new Error("Enter the mind's whole address, like example.com.");
  const candidates = [`https://${PLATFORM_LABEL}.${host}`];
  if (host.startsWith(`${PLATFORM_LABEL}.`) && host.slice(PLATFORM_LABEL.length + 1).includes("."))
    candidates.push(`https://${host}`);
  return candidates;
}

interface IssuerMetadata {
  issuer: string;
  authorization_endpoint: string;
  token_endpoint: string;
  protected_resources?: string[];
}

/** The mind's issuer: the first candidate whose discovery document names itself and its `/api`. */
export async function discoverMind(
  typed: string,
  fetcher: typeof fetch,
): Promise<{ platformOrigin: string; metadata: IssuerMetadata }> {
  for (const origin of platformOriginCandidates(typed)) {
    const response = await fetcher(`${origin}/.well-known/oauth-authorization-server`, {
      headers: { accept: "application/json" },
    }).catch(() => null);
    if (!response?.ok) continue;
    const metadata = (await response.json().catch(() => null)) as IssuerMetadata | null;
    if (
      metadata?.issuer === origin &&
      typeof metadata.authorization_endpoint === "string" &&
      typeof metadata.token_endpoint === "string" &&
      metadata.protected_resources?.includes(`${origin}/api`)
    )
      return { platformOrigin: origin, metadata };
  }
  throw new Error(`${typed.trim()} has no iterate mind that Kit can find.`);
}

const base64url = (bytes: Uint8Array) =>
  btoa(String.fromCharCode(...bytes))
    .replace(/\+/g, "-")
    .replace(/\//g, "_")
    .replace(/=+$/, "");

/** Each board is its own OAuth client, named before consent (src/firmware/device-client.ts). */
export function mindClientOf(device: FirmwareDevice, kitOrigin: string, id = crypto.randomUUID()) {
  return {
    clientId: `${kitOrigin}/devices/${device.id}/clients/${id}.json`,
    redirectUri: `${kitOrigin}/devices/${device.id}/mind`,
  };
}

/** Starts the sign-in: returns the issuer's authorize URL, to navigate to. */
export async function startMindSignIn(input: {
  device: FirmwareDevice;
  address: string;
  kitOrigin: string;
  storage: Storage;
  fetcher: typeof fetch;
}): Promise<string> {
  const { platformOrigin, metadata } = await discoverMind(input.address, input.fetcher);
  const { clientId, redirectUri } = mindClientOf(input.device, input.kitOrigin);
  const verifier = base64url(crypto.getRandomValues(new Uint8Array(32)));
  const challenge = base64url(
    new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(verifier))),
  );
  const state = base64url(crypto.getRandomValues(new Uint8Array(16)));
  const pending: PendingSignIn = {
    state,
    verifier,
    address: input.address.trim(),
    platformOrigin,
    tokenEndpoint: metadata.token_endpoint,
    clientId,
    redirectUri,
  };
  input.storage.setItem(pendingKey(input.device.id), JSON.stringify(pending));
  const url = new URL(metadata.authorization_endpoint);
  url.search = new URLSearchParams({
    response_type: "code",
    client_id: clientId,
    redirect_uri: redirectUri,
    scope: "iterate",
    resource: `${platformOrigin}/api`,
    state,
    code_challenge: challenge,
    code_challenge_method: "S256",
  }).toString();
  return url.href;
}

/** Ends the sign-in the issuer sent back here: exchanges the code for the board's grant. */
export async function finishMindSignIn(input: {
  device: FirmwareDevice;
  code: string;
  state: string;
  storage: Storage;
  fetcher: typeof fetch;
}): Promise<MindSignIn> {
  const saved = input.storage.getItem(pendingKey(input.device.id));
  input.storage.removeItem(pendingKey(input.device.id));
  const pending = saved ? (JSON.parse(saved) as PendingSignIn) : null;
  if (!pending || pending.state !== input.state)
    throw new Error("This sign-in did not start on this page. Start again.");
  const response = await input.fetcher(pending.tokenEndpoint, {
    method: "POST",
    headers: { "content-type": "application/x-www-form-urlencoded", accept: "application/json" },
    body: new URLSearchParams({
      grant_type: "authorization_code",
      code: input.code,
      redirect_uri: pending.redirectUri,
      client_id: pending.clientId,
      code_verifier: pending.verifier,
      resource: `${pending.platformOrigin}/api`,
    }),
  });
  const answer = (await response.json().catch(() => null)) as {
    refresh_token?: string;
    error?: string;
    error_description?: string;
  } | null;
  if (!response.ok || !answer?.refresh_token)
    throw new Error(
      `${pending.address} did not give the device a sign-in: ${answer?.error_description || answer?.error || `HTTP ${response.status}`}`,
    );
  return {
    address: pending.address,
    platformOrigin: pending.platformOrigin,
    clientId: pending.clientId,
    refreshToken: answer.refresh_token,
  };
}
