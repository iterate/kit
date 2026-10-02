import handler, { createServerEntry } from "@tanstack/react-start/server-entry";
import { env } from "cloudflare:workers";
import { appAuth, issuerAnswersAt } from "iterate/app-server";
import type { BrowserSession } from "iterate/app-session";
import { kitConfigOf } from "./app/config.ts";
import { proxyPosthogRequest } from "./app/posthog-proxy.ts";
import { deviceClientMetadata } from "./firmware/device-client.ts";
import { proxyFirmwareFile } from "./firmware/firmware-proxy.ts";
import { deviceAuth } from "./device-auth.ts";
export { BrowserSession } from "iterate/app-session";

declare global {
  namespace Cloudflare {
    /** The bindings vite.config.ts `workerConfig` gives the Worker. */
    interface Env {
      ASSETS: Fetcher;
      BROWSER_SESSION: DurableObjectNamespace<BrowserSession>;
      /** THE WORKER'S CONFIGURATION, JSON (src/app/config.ts), from envs.ts. */
      APP_CONFIG: string;
    }
  }
}

/** THE WORKER. In order: the health check, PostHog through our own origin, Kit's own public routes
 *  (firmware files and each device's OAuth client), the sign-in gate and the authenticated /api,
 *  then the built assets and TanStack Start's pages. It is the shared app server of iterate's
 *  monorepo (its packages/ui/src/apps/server.ts at iterate/iterate@a5a07e8) with Kit's routes in
 *  its `before` slot.
 *
 *  Every device is its own OAuth client (device-auth.ts): `deviceAuth` answers `/.auth/login` and
 *  `/.auth/connect` ahead of the gate, so no Kit sign-in uses the generic `/.auth/client.json` the
 *  gate still publishes. */
export default createServerEntry({
  async fetch(request: Request) {
    // parsed on the first request, /healthz's included: a malformed config fails the deploy's smoke
    const config = kitConfigOf(env);
    const url = new URL(request.url);
    if (url.pathname === "/healthz") return new Response("ok");
    // posthog-js's `api_host` (src/components/posthog.tsx): PostHog EU through our own origin
    if (url.pathname.startsWith("/e/")) return proxyPosthogRequest({ request, proxyPrefix: "/e" });
    // Firmware release files are public: esp-web-tools fetches them from the page (firmware-proxy.ts).
    const firmware = await proxyFirmwareFile(request, fetch);
    if (firmware) return firmware;
    const own =
      deviceClientMetadata(url) ||
      (await deviceAuth(
        request,
        {
          sessions: env.BROWSER_SESSION,
          defaultIssuer: config.urls.os,
          denyZones: config.denyZones,
        },
        { issuerAnswersAt },
      ));
    if (own) return own;
    const auth = await appAuth(request, {
      sessions: env.BROWSER_SESSION,
      issuer: config.urls.os,
      resource: `${config.urls.os}/api`,
      denyZones: config.denyZones,
      api: (request) => fetch(request),
    });
    if (auth) return auth;
    const asset = await env.ASSETS.fetch(request);
    if (asset.status !== 404) return asset;
    return handler.fetch(request);
  },
});
