import { readdirSync } from "node:fs";
import { cloudflare } from "@cloudflare/vite-plugin";
import { tanstackStart } from "@tanstack/react-start/plugin/vite";
import tailwindcss from "@tailwindcss/vite";
import viteReact from "@vitejs/plugin-react";
import { COMPATIBILITY_DATE } from "iterate/compatibility-date";
import { defineConfig } from "vite";
import type { z } from "zod";
import type { KitConfig } from "./src/app/config.ts";
import { DENY_ZONES, getEnv, kitEnvs } from "./envs.ts";
import { routeTreeStyle } from "./scripts/app.ts";

export default defineConfig({
  plugins: [
    cloudflare({
      viteEnvironment: { name: "ssr" },
      config: workerConfig(process.env.CLOUDFLARE_ENV),
    }),
    tanstackStart({ router: routeTreeStyle, importProtection: { behavior: "error" } }),
    viteReact(),
    tailwindcss(),
  ],
});

/** THE WORKER'S CONFIG for an envs.ts env (CLOUDFLARE_ENV, which `pnpm deploy` sets), or for local
 *  dev with none: prd's links, which a gitignored `.dev.vars` overrides
 *  (`APP_CONFIG_URLS__OS=http://localhost:8788`). `vite build` snapshots it into
 *  dist/server/wrangler.json, what a deploy ships. */
function workerConfig(envName: string | undefined) {
  const env = envName ? getEnv(envName) : undefined;
  const links = env || kitEnvs.prd;
  // THE APP'S CONFIGURATION (src/app/config.ts documents each key)
  const appConfig = {
    urls: { os: links.os, dash: links.dash },
    denyZones: DENY_ZONES,
    ...(env?.posthogProjectKey && { posthogProjectKey: env.posthogProjectKey }),
  } satisfies z.input<typeof KitConfig>;
  const hostname = env && new URL(env.baseUrl).hostname;
  return {
    name: env?.workerName || "kit",
    main: "src/server.ts",
    // the date the iterate SDK's Workers are tested at
    compatibility_date: COMPATIBILITY_DATE,
    compatibility_flags: ["nodejs_compat", "global_fetch_strictly_public"],
    durable_objects: { bindings: [{ name: "BROWSER_SESSION", class_name: "BrowserSession" }] },
    exports: { BrowserSession: { type: "durable-object" as const, storage: "sqlite" as const } },
    vars: { APP_CONFIG: JSON.stringify(appConfig) },
    observability: {
      enabled: true,
      head_sampling_rate: 1,
      logs: { enabled: true, head_sampling_rate: 1, persist: true, invocation_logs: true },
      traces: { enabled: true, persist: true, head_sampling_rate: 1 },
    },
    assets: {
      binding: "ASSETS",
      not_found_handling: "none" as const,
      // THE REQUESTS THAT START THE WORKER: every one but the static files, which the asset worker
      // answers without starting an isolate (a cold isolate cost a static file 68–274 ms on prd,
      // 2026-09-24): vite's hashed output under /assets/, and each top-level entry of public/,
      // which vite copies to the build's root.
      // https://developers.cloudflare.com/workers/static-assets/binding/#run_worker_first
      run_worker_first: [
        "/*",
        "!/assets/*",
        ...readdirSync(new URL("public/", import.meta.url), { withFileTypes: true }).map((entry) =>
          entry.isDirectory() ? `!/${entry.name}/*` : `!/${entry.name}`,
        ),
      ],
    },
    ...(env && { account_id: env.cloudflareAccountId, workers_dev: true }),
    // A custom domain gets a route on its zone; a workers.dev baseUrl is served by workers_dev.
    ...(hostname &&
      !hostname.endsWith(".workers.dev") && {
        routes: [{ pattern: `${hostname}/*`, zone_name: hostname.split(".").slice(-2).join(".") }],
      }),
  };
}
