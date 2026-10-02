// config.ts — THE WORKER'S CONFIGURATION, its one `APP_CONFIG` var, written from envs.ts
// (vite.config.ts `workerConfig`), with any key set alone as an `APP_CONFIG_*` var merged on top (the SDK's
// `parseAppConfigVars`). Kit holds no secrets, so nothing comes from Doppler. Local dev starts from
// prd's and names a local platform in a gitignored `.dev.vars`:
//
//   APP_CONFIG_URLS__OS=http://localhost:8788
//
// A trimmed copy of iterate's StartAppConfig (packages/shared/src/start-app-config.ts at
// iterate/iterate@a5a07e8), which every app in its monorepo shares: Kit reads only these keys.

import { z } from "zod";
import { dnsName, httpOrigin, optionalOrigin, parseAppConfigVars } from "iterate/app-config";

export const KitConfig = z.object({
  urls: z
    .object({
      /** THE PLATFORM Kit signs in against: the default issuer, and `/api` on it the resource. A
       *  device can also sign in to another platform a link names (device-auth.ts). */
      os: httpOrigin,
      /** Its dash, where a person revokes a device's token. Blank ⇒ no link. */
      dash: optionalOrigin,
    })
    // the prefault must satisfy the input type; `os: ""` then fails naming urls.os
    .prefault({ os: "" }),
  /** ITERATE'S OWN ZONES (envs.ts): project hosts and custom apexes are userspace and could serve a
   *  look-alike issuer, so the browser-auth gate (`appAuth` `denyZones`, the SDK's
   *  `issuerOriginOf`) refuses to connect to an issuer under one. The default issuer, `urls.os`,
   *  is exempt. */
  denyZones: z.array(dnsName),
  /** PostHog's project key (envs.ts, prd only): the pages start posthog-js with it. Blank ⇒ no
   *  PostHog. */
  posthogProjectKey: z.string().trim().default(""),
});

export type KitConfig = z.output<typeof KitConfig>;

const kitConfigByEnv = new WeakMap<object, KitConfig>();

/** The configuration of the isolate `env` belongs to (`import { env } from "cloudflare:workers"`):
 *  parsed on first use, then the same object every time. A malformed field throws HERE, on the
 *  first request, naming the field. */
export function kitConfigOf(env: object): KitConfig {
  let config = kitConfigByEnv.get(env);
  if (!config) {
    config = parseAppConfigVars(env, KitConfig);
    kitConfigByEnv.set(env, config);
  }
  return config;
}
