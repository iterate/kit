// The root route's document and head, as every app in iterate's monorepo has them: copied from its
// packages/ui/src/apps/{document.tsx,providers.tsx,head.ts} at iterate/iterate@a5a07e8, less the
// base path that only apps served under a project's hosts use.
import { Outlet, Scripts, useHydrated } from "@tanstack/react-router";
import type { ReactNode } from "react";
import { EnvironmentHeadContent } from "#/components/environment-head-content.tsx";
import { initPosthog } from "#/components/posthog.tsx";
import { Toaster } from "#/components/ui/sonner.tsx";
import { TooltipProvider } from "#/components/ui/tooltip.tsx";

/** The root route's `head`: the title, the stylesheet (a `?url` import) and the description. */
export function appHead(page: { title: string; stylesheet: string; description: string }) {
  return {
    meta: [
      { charSet: "utf-8" },
      { name: "viewport", content: "width=device-width, initial-scale=1" },
      { name: "description", content: page.description },
      { title: page.title },
    ],
    links: [{ rel: "stylesheet", href: page.stylesheet }],
  };
}

/** The root route component: the document, with the providers around the pages. `icon` is the
 *  icon file in production. */
export function AppDocument(props: { icon: string; posthogProjectKey: string | null }) {
  // false in the server's HTML, true once React owns the page
  const hydrated = useHydrated();
  return (
    <html lang="en">
      <head>
        <EnvironmentHeadContent productionIcon={props.icon} />
      </head>
      <body className="min-h-svh bg-background font-sans antialiased" data-hydrated={hydrated}>
        <AppProviders posthogApiKey={props.posthogProjectKey || undefined}>
          <Outlet />
        </AppProviders>
        <Scripts />
      </body>
    </html>
  );
}

/** PostHog starts when the deployment has a key (envs.ts hands one to prd only). */
function AppProviders(props: { children: ReactNode; posthogApiKey?: string }) {
  initPosthog(props.posthogApiKey);

  return (
    // Base UI tooltips are intended to share a provider; delay=0 makes hover tooltips immediate.
    // https://github.com/mui/base-ui/blob/master/docs/src/app/(docs)/react/components/tooltip/page.mdx
    <TooltipProvider delay={0}>
      {props.children}
      {/* Light mode only. The vendored Toaster asks next-themes for the theme, and with no
          ThemeProvider mounted it falls back to "system", which follows the OS into dark; a `theme`
          prop overrides it (it spreads the props last). */}
      <Toaster theme="light" />
    </TooltipProvider>
  );
}
