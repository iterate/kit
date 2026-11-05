import type { ReactNode } from "react";
import { IterateLogo } from "#/components/iterate-logo.tsx";
import { Button } from "#/components/ui/button.tsx";
import { Input } from "#/components/ui/input.tsx";
import { FIRMWARE_REPOSITORY } from "../firmware/catalog.ts";

/** The installer pages' frame: a slim header that names the platform in use and switches it, the
 *  page, and a footer with the repository. Switching goes through Kit's connect route
 *  (device-auth.ts), which checks the platform and returns to device selection, so the server stays
 *  the one place that decides which issuers are allowed. */
export function KitChrome(props: {
  /** the issuer in use: the deployment's default, or the platform a connect link named */
  platformOrigin: string;
  /** the deployment's default issuer (`urls.os`) */
  defaultIssuer: string;
  /** the selected device, kept across a platform switch */
  deviceId?: string;
  children: ReactNode;
}) {
  const platformHost = new URL(props.platformOrigin).host;
  const isDefault = new URL(props.platformOrigin).origin === new URL(props.defaultIssuer).origin;
  const next = props.deviceId ? `/devices/${props.deviceId}` : undefined;
  return (
    <div className="flex min-h-svh flex-col">
      <header className="flex flex-wrap items-center justify-between gap-x-6 gap-y-2 border-b px-5 py-3 text-sm sm:px-8">
        <a href="/" className="flex items-center gap-2 font-semibold tracking-tight">
          <IterateLogo className="size-6" />
          Kit
        </a>
        <div className="flex flex-wrap items-center gap-x-3 gap-y-2">
          <span className="text-muted-foreground">
            Platform <span className="font-medium text-foreground">{platformHost}</span>
          </span>
          {/* A plain GET form, so it works before the page hydrates (with a full origin typed);
              once hydrated, a bare host gets https:// in front. */}
          <form
            method="get"
            action="/.auth/connect"
            className="flex items-center gap-2"
            onSubmit={(event) => {
              event.preventDefault();
              const typed = String(new FormData(event.currentTarget).get("issuer") || "").trim();
              if (!typed) return;
              const issuer = /^[a-z][a-z0-9+.-]*:\/\//i.test(typed) ? typed : `https://${typed}`;
              const query = new URLSearchParams({ issuer });
              if (next) query.set("next", next);
              window.location.assign(`/.auth/connect?${query}`);
            }}
          >
            {next && <input type="hidden" name="next" value={next} />}
            <Input
              name="issuer"
              aria-label="Another platform's host"
              placeholder="iterate.example.com"
              inputMode="url"
              autoCapitalize="none"
              autoCorrect="off"
              spellCheck={false}
              required
              className="h-7 w-44"
            />
            <Button type="submit" variant="outline" size="sm">
              Switch
            </Button>
          </form>
          {!isDefault && (
            <a
              href={next ? `/.auth/connect?${new URLSearchParams({ next })}` : "/"}
              className="text-xs underline underline-offset-4"
            >
              Use {new URL(props.defaultIssuer).host}
            </a>
          )}
        </div>
      </header>
      <div className="flex flex-1 flex-col">{props.children}</div>
      <footer className="flex flex-wrap items-center justify-center gap-x-4 gap-y-1 border-t px-5 py-4 text-xs text-muted-foreground">
        <span>Kit is open source.</span>
        <a
          href={`https://github.com/${FIRMWARE_REPOSITORY}`}
          className="underline underline-offset-4"
        >
          Source on GitHub
        </a>
        <a
          href={`https://github.com/${FIRMWARE_REPOSITORY}/releases`}
          className="underline underline-offset-4"
        >
          Firmware releases
        </a>
      </footer>
    </div>
  );
}
