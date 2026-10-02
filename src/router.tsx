import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { createRouter } from "@tanstack/react-router";
import {
  DefaultErrorComponent,
  DefaultNotFoundComponent,
  DefaultPendingComponent,
} from "#/components/route-defaults.tsx";
import { routeTree } from "./routeTree.gen.ts";

// routeTree.gen.ts registers `router: ReturnType<typeof getRouter>` on Start's Register interface,
// so this function's inferred return type IS the app's router type. The defaults are the ones every
// app in iterate's monorepo shares (its packages/ui/src/apps/router.tsx).
export function getRouter() {
  // one per router, like the router itself: the server builds one per request
  const queryClient = new QueryClient();
  return createRouter({
    routeTree,
    defaultPreload: "intent",
    scrollRestoration: true,
    // Components passed as options are wrapped in lambdas so checking them doesn't traverse the
    // registered router types (TS7023).
    defaultErrorComponent: (props) => <DefaultErrorComponent {...props} />,
    defaultNotFoundComponent: () => <DefaultNotFoundComponent />,
    // Without a default pending component, an `ssr: false` route — the whole signed-in `_auth`
    // layout — renders a BLANK outlet in the SSR shell and again while `beforeLoad`/`loader` run on
    // the client.
    defaultPendingComponent: () => <DefaultPendingComponent />,
    // Show that feedback quickly on client-side loads too: the library defaults (1000ms before
    // pending shows, 500ms minimum once shown) leave a full second of blank panel.
    defaultPendingMs: 300,
    defaultPendingMinMs: 200,
    Wrap: ({ children }) => (
      <QueryClientProvider client={queryClient}>{children}</QueryClientProvider>
    ),
  });
}
declare module "@tanstack/react-router" {
  interface Register {
    router: ReturnType<typeof getRouter>;
  }
}
