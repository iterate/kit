/** Kit's deployments. vite.config.ts turns one into the Worker's config (there is no wrangler
 *  file), and `pnpm deploy --env <name>` ships it (scripts/app.ts). Secrets live in Doppler project
 *  `kit`, one config per deployment; the deploy reads its Cloudflare token there.
 *
 *  Plain data, importing nothing: scripts/firmware-release.ts imports it under plain `node` before
 *  anything is installed, and the Worker imports it too (src/device-auth.ts). */

export interface KitEnv {
  cloudflareAccountId: string;
  /** Doppler config (project `kit`) supplying deploy credentials. */
  dopplerConfig: string;
  workerName: string;
  baseUrl: string;
  /** The iterate platform Kit signs in against: the default issuer, and `/api` on it the resource.
   *  A device can sign in to another platform a link names (src/device-auth.ts). */
  os: string;
  /** The dash of that platform, where a person revokes a device's token. */
  dash: string;
  /** PostHog's project key ("iterate (prd)" in PostHog EU; public, it ships in every page). Unset ⇒
   *  no PostHog. */
  posthogProjectKey?: string;
}

/** The two Cloudflare accounts: production (iterate.com's zones), and dev/preview. */
const PRD_ACCOUNT_ID = "04b3b57291ef2626c6a8daa9d47065a7";
const PREVIEW_AND_DEV_ACCOUNT_ID = "376ef7ed81b0573f93524de763666c15";

export const kitEnvs = {
  // KIT AT MAIN on the dev/preview account, signed in against iterate's main on dev. Every Kit lists
  // and flashes the same GitHub releases as production (src/firmware/releases.ts).
  preview: {
    cloudflareAccountId: PREVIEW_AND_DEV_ACCOUNT_ID,
    dopplerConfig: "preview",
    workerName: "kit",
    baseUrl: "https://kit.iterate-dev-preview.workers.dev",
    os: "https://os.iterate-dev-preview.workers.dev",
    dash: "https://dash.iterate-dev-preview.workers.dev",
  },
  prd: {
    cloudflareAccountId: PRD_ACCOUNT_ID,
    dopplerConfig: "prd",
    // The production account's workers.dev subdomain is `iterate`, making this worker available at
    // kiterate.iterate.workers.dev as well.
    workerName: "kiterate",
    baseUrl: "https://k.iterate.com",
    os: "https://os.iterate.com",
    dash: "https://dash.iterate.com",
    posthogProjectKey: "phc_2MGb9SEJABGj4sCx4grFIbzMR7NjbcUgP5YmhSXfcr7",
  },
} satisfies Record<string, KitEnv>;

export function getEnv(name: string): KitEnv {
  const env = (kitEnvs as Record<string, KitEnv>)[name];
  if (!env)
    throw new Error(
      `unknown env ${JSON.stringify(name)}; known envs: ${Object.keys(kitEnvs).join(", ")}`,
    );
  return env;
}

/** ITERATE'S OWN ZONES, which Kit refuses as a device's issuer (`denyZones`): a project host or a
 *  custom apex is userspace and could serve a look-alike issuer. A copy of iterate's `ownZones()`
 *  (its scripts/lib/start-app.ts, from its envs.ts) at iterate/iterate@a5a07e8, when Kit moved here.
 *  When iterate adds a zone of its own, add it here too. */
export const DENY_ZONES = [
  "admin.iterate-dev-preview.workers.dev",
  "agents.iterate-dev-preview.workers.dev",
  "dash.iterate-dev-preview.workers.dev",
  "docs.iterate-dev-preview.workers.dev",
  "docs.iterate.workers.dev",
  ...Array.from({ length: 19 }, (_, i) => `iterate-preview-${i + 1}.app`),
  "iterate.app",
  "iterate.com",
  "kit.iterate-dev-preview.workers.dev",
  "notes.iterate-dev-preview.workers.dev",
  "os.iterate-dev-preview.workers.dev",
  "voice.iterate-dev-preview.workers.dev",
];
