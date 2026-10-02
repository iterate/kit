import { createRootRoute } from "@tanstack/react-router";
import { createServerFn } from "@tanstack/react-start";
import { AppDocument, appHead } from "#/app/document.tsx";
import { kitConfigOf } from "#/app/config.ts";
import css from "../styles.css?url";
/** What the worker's `APP_CONFIG` says about this deployment: its PostHog project key (envs.ts, prd
 *  only) and its dash, where a person revokes a device's token (`urls.dash`). `dashOrigin` is null
 *  when it names none. */
const deployment = createServerFn().handler(async () => {
  const { env } = await import("cloudflare:workers");
  const config = kitConfigOf(env);
  return {
    posthogProjectKey: config.posthogProjectKey || null,
    dashOrigin: config.urls.dash || null,
  };
});

export const Route = createRootRoute({
  loader: () => deployment(),
  staleTime: Infinity,
  head: () =>
    appHead({
      title: "iterate Kit",
      stylesheet: css,
      description: "Install and configure an iterate voice device from your browser.",
    }),
  component: Root,
});

function Root() {
  const { posthogProjectKey } = Route.useLoaderData();
  return <AppDocument icon="/favicon.svg" posthogProjectKey={posthogProjectKey} />;
}
