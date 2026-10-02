# iterate Kit

Firmware for iterate's voice boards (ESP-IDF 6.1, and the Mac as a board), and the browser
installer at k.iterate.com (TanStack Start on a Cloudflare Worker). Read [README.md](README.md)
first, and [firmware/AGENTS.md](firmware/AGENTS.md) before touching `firmware/`.

- Before pushing: `pnpm typecheck && pnpm lint && pnpm format && pnpm test`, and
  `pnpm firmware:test:host` after a firmware change (needs cmake).
- Never hand-edit `src/routeTree.gen.ts`: `pnpm routes:generate`.
- `envs.ts` owns the deployments; Doppler project `kit` holds the deploy credentials. Main deploys
  itself (`.depot/workflows/deploy.yml`). Workers are never deleted.
- The repository stays public: boards and browsers download its firmware releases anonymously.
  Firmware versions and the release contract: README.md "Firmware releases".
- `src/components/ui/` is shadcn's, byte for byte: re-add a component with
  `pnpm exec shadcn add <name>` instead of editing it. iterate's own components come from
  `iterate/packages/<name>` the same way.
- Code copied from iterate's monorepo says so at the top, with the commit it came from.
- New React code: no `useEffect`, and `useState` only as a last resort; server state is TanStack
  Query.
