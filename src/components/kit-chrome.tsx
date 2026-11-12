import type { ReactNode } from "react";
import { IterateLogo } from "#/components/iterate-logo.tsx";
import { FIRMWARE_REPOSITORY } from "../firmware/catalog.ts";

/** The installer pages' frame: a slim header, the page, and a footer with the repository. A board
 *  is set up for a mind (routes/devices.$deviceId.mind.tsx): there is no platform to choose. */
export function KitChrome(props: { children: ReactNode }) {
  return (
    <div className="flex min-h-svh flex-col">
      <header className="flex flex-wrap items-center justify-between gap-x-6 gap-y-2 border-b px-5 py-3 text-sm sm:px-8">
        <a href="/" className="flex items-center gap-2 font-semibold tracking-tight">
          <IterateLogo className="size-6" />
          Kit
        </a>
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
