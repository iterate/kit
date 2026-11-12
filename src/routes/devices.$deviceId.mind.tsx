import { skipToken, useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { createFileRoute, getRouteApi, notFound } from "@tanstack/react-router";
import { UsbIcon } from "lucide-react";
import { z } from "zod";
import { IterateLogo } from "#/components/iterate-logo.tsx";
import { KitChrome } from "#/components/kit-chrome.tsx";
import { Button } from "#/components/ui/button.tsx";
import { Field, FieldContent, FieldDescription, FieldLabel } from "#/components/ui/field.tsx";
import { Input } from "#/components/ui/input.tsx";
import { Spinner } from "#/components/ui/spinner.tsx";
import { SetupWizard, type SetupInput } from "../components/setup-wizard.tsx";
import {
  horizontalFieldClassName,
  readWifiFields,
  WifiFields,
} from "../components/wifi-fields.tsx";
import {
  FIRMWARE_REPOSITORY,
  findFirmwareDevice,
  firmwareReleaseTag,
} from "../firmware/catalog.ts";
import type { DeviceConfiguration } from "../firmware/config-image.ts";
import { finishMindSignIn, type MindSignIn, startMindSignIn } from "../firmware/mind-sign-in.ts";
import { newestFirmware } from "../firmware/releases.ts";

/**
 * SETTING A BOARD UP FOR A MIND (iterate/core). The person names the mind, signs in there and
 * approves this board, and the issuer sends the browser back here with a code; the page trades it
 * for the board's own grant (src/firmware/mind-sign-in.ts) and writes the grant's refresh token
 * into the board with the firmware. Nothing here goes through Kit's Worker, which has no session
 * for a mind.
 *
 * The mind must already run voice: its config installs `@iterate-com/voice` (`installVoice`) and
 * holds an OpenAI key. The board provisions its own two voice contexts when it connects
 * (`itx.voice.ensureVoiceDevice`), so there is nothing to prepare from here.
 */
export const Route = createFileRoute("/devices/$deviceId/mind")({
  ssr: false,
  validateSearch: z.object({
    address: z.string().optional().catch(undefined),
    code: z.string().optional().catch(undefined),
    state: z.string().optional().catch(undefined),
    error: z.string().optional().catch(undefined),
    error_description: z.string().optional().catch(undefined),
  }),
  beforeLoad: ({ params }) => {
    const device = findFirmwareDevice(params.deviceId);
    if (!device) throw notFound();
    return { device };
  },
  loader: async ({ context }) => ({ firmware: await newestFirmware(context.device, fetch) }),
  staleTime: Infinity,
  head: () => ({ meta: [{ title: "Set up your device for a mind · Kit" }] }),
  component: MindSetup,
});

/** The root loader's: this deployment's default platform (routes/__root.tsx). */
const root = getRouteApi("__root__");

interface MindSetupInput extends SetupInput {
  signIn: MindSignIn;
}

function MindSetup() {
  const search = Route.useSearch();
  const navigate = Route.useNavigate();
  const { device } = Route.useRouteContext();
  const { firmware } = Route.useLoaderData();
  const { defaultIssuer } = root.useLoaderData();
  const queryClient = useQueryClient();
  const signInKey = ["kit", "mind-sign-in", device.id];
  // The board's grant, once the issuer sent the browser back: held in this page's memory only.
  const signIn = useQuery<MindSignIn>({ queryKey: signInKey, queryFn: skipToken });
  // The issuer's redirect, finished once per code; the code then leaves the address bar.
  const finishing = useQuery({
    queryKey: ["kit", "mind-sign-in-code", device.id, search.code],
    queryFn:
      search.code && search.state
        ? async () => {
            const finished = await finishMindSignIn({
              device,
              code: search.code!,
              state: search.state!,
              storage: sessionStorage,
              fetcher: fetch,
            });
            queryClient.setQueryData(signInKey, finished);
            void navigate({ search: { address: finished.address }, replace: true });
            return true;
          }
        : skipToken,
    retry: false,
    staleTime: Infinity,
  });
  const starting = useMutation({
    mutationFn: async (address: string) => {
      const authorize = await startMindSignIn({
        device,
        address,
        kitOrigin: window.location.origin,
        storage: sessionStorage,
        fetcher: fetch,
      });
      window.location.assign(authorize);
    },
  });
  const preparing = useMutation({
    mutationFn: async (input: MindSetupInput): Promise<DeviceConfiguration> => ({
      wifi: input.wifi,
      iterate: {
        baseUrl: input.signIn.platformOrigin,
        clientId: input.signIn.clientId,
        refreshToken: input.signIn.refreshToken,
      },
      statusVoice: input.statusVoice,
    }),
  });
  const webSerial = "serial" in navigator;
  const refused = search.error ? search.error_description || search.error : null;

  return (
    <KitChrome platformOrigin={defaultIssuer} defaultIssuer={defaultIssuer} deviceId={device.id}>
      <div className="mx-auto flex w-full max-w-5xl flex-1 px-5 py-10 sm:px-8 sm:py-16">
        <main className="grid w-full gap-10 lg:grid-cols-[minmax(0,0.8fr)_minmax(28rem,1.2fr)] lg:gap-16">
          <section className="flex max-w-sm flex-col gap-6">
            <header className="flex items-center gap-3">
              <IterateLogo className="size-9" />
              <h1 className="text-xl font-semibold tracking-tight">
                Set up your device for a mind
              </h1>
            </header>
            <div className="flex flex-col gap-3 text-sm leading-relaxed text-muted-foreground">
              <p>
                Sign in to your mind and approve this device. The device gets its own sign-in, which
                it renews while it is on. If it stays off for more than 7 days, set it up again.
              </p>
              <p>
                Your mind must already run voice. The device sets up its own voice agent there when
                it first connects.
              </p>
              <p>The sign-in never leaves this page except to the device.</p>
            </div>
          </section>

          <section aria-label="Device configuration" className="flex flex-col gap-6">
            <Field className={horizontalFieldClassName}>
              <FieldLabel className="sm:pt-2">Device</FieldLabel>
              <FieldContent>
                <span className="sm:pt-2">{device.name}</span>
                <FieldDescription className="flex items-center gap-2">
                  <img
                    src={`/vendors/${device.vendor.icon}`}
                    alt={device.vendor.name}
                    className="size-6 object-contain"
                  />
                  {device.description}
                </FieldDescription>
              </FieldContent>
            </Field>

            <Field className={horizontalFieldClassName}>
              <FieldLabel className="sm:pt-2">Firmware</FieldLabel>
              <FieldContent>
                {firmware.manifest ? (
                  <>
                    <span className="sm:pt-2">{firmware.manifest.version}</span>
                    <FieldDescription>
                      The newest release for this device.{" "}
                      <a
                        href={`https://github.com/${FIRMWARE_REPOSITORY}/releases/tag/${firmwareReleaseTag(device.id, firmware.manifest.version)}`}
                        target="_blank"
                        rel="noreferrer"
                      >
                        Release notes
                      </a>
                    </FieldDescription>
                  </>
                ) : (
                  <p role="alert" data-type="error" className="text-sm text-destructive sm:pt-2">
                    {firmware.problem}
                  </p>
                )}
              </FieldContent>
            </Field>

            {finishing.isFetching ? (
              <div className="flex items-center gap-2 text-sm text-muted-foreground">
                <Spinner /> Getting the device’s sign-in…
              </div>
            ) : signIn.data ? (
              <form
                className="flex flex-col gap-6"
                onSubmit={(event) => {
                  event.preventDefault();
                  const { wifi, statusVoice } = readWifiFields(new FormData(event.currentTarget));
                  preparing.mutate({
                    signIn: signIn.data,
                    target: { kind: "mind", name: signIn.data.address },
                    wifi,
                    statusVoice,
                  });
                }}
              >
                <Field className={horizontalFieldClassName}>
                  <FieldLabel className="sm:pt-2">Mind</FieldLabel>
                  <FieldContent>
                    <span className="sm:pt-2">{signIn.data.address}</span>
                    <FieldDescription>
                      Signed in. End the device’s sign-in from the mind’s keys page at{" "}
                      {new URL(signIn.data.platformOrigin).host}.
                    </FieldDescription>
                  </FieldContent>
                </Field>
                <WifiFields />
                <div className="grid sm:grid-cols-[8.5rem_minmax(0,1fr)] sm:gap-4">
                  <div className="flex flex-col gap-2 sm:col-start-2">
                    <Button
                      className="w-full"
                      type="submit"
                      disabled={!webSerial || !firmware.manifest}
                    >
                      <UsbIcon data-icon="inline-start" />
                      Flash device
                    </Button>
                    {!webSerial && (
                      <p role="alert" data-type="error" className="text-xs text-destructive">
                        This browser can’t reach USB devices (it has no Web Serial). Open this page
                        in Chrome or Edge on a computer.
                      </p>
                    )}
                  </div>
                </div>
              </form>
            ) : (
              <form
                className="flex flex-col gap-6"
                onSubmit={(event) => {
                  event.preventDefault();
                  starting.mutate(String(new FormData(event.currentTarget).get("address") || ""));
                }}
              >
                <Field className={horizontalFieldClassName}>
                  <FieldLabel htmlFor="address" className="sm:pt-2">
                    Mind
                  </FieldLabel>
                  <FieldContent>
                    <Input
                      id="address"
                      name="address"
                      placeholder="example.com"
                      defaultValue={search.address || ""}
                      autoComplete="url"
                      required
                    />
                    <FieldDescription>Your mind’s address, its domain.</FieldDescription>
                  </FieldContent>
                </Field>
                {(refused || finishing.error || starting.error) && (
                  <p role="alert" data-type="error" className="text-sm text-destructive">
                    {refused
                      ? `The mind did not approve the device: ${refused}`
                      : (finishing.error || starting.error)!.message}
                  </p>
                )}
                <div className="grid sm:grid-cols-[8.5rem_minmax(0,1fr)] sm:gap-4">
                  <Button
                    className="w-full sm:col-start-2"
                    type="submit"
                    disabled={starting.isPending}
                  >
                    Sign in to the mind
                  </Button>
                </div>
              </form>
            )}
            {firmware.manifest && (
              <SetupWizard preparing={preparing} device={device} manifest={firmware.manifest} />
            )}
          </section>
        </main>
      </div>
    </KitChrome>
  );
}
