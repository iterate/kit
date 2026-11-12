import { useState } from "react";
import { createFileRoute } from "@tanstack/react-router";
import { z } from "zod";
import { IterateLogo } from "#/components/iterate-logo.tsx";
import { KitChrome } from "#/components/kit-chrome.tsx";
import { Button } from "#/components/ui/button.tsx";
import { Field, FieldContent, FieldLabel, FieldDescription } from "#/components/ui/field.tsx";
import { Input } from "#/components/ui/input.tsx";
import {
  Select,
  SelectContent,
  SelectGroup,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "#/components/ui/select.tsx";
import { DEFAULT_DEVICE_ID, findFirmwareDevice, firmwareCatalog } from "../firmware/catalog.ts";

export const Route = createFileRoute("/")({
  validateSearch: z.object({
    device: z.string().optional().catch(undefined),
    // another iterate platform (a self-hosted one), carried here by `/.auth/connect?issuer=`
    // (device-auth.ts, which checks it again when the login is posted)
    issuer: z
      .url({ protocol: /^https$/ })
      .optional()
      .catch(undefined),
  }),
  head: () => ({ meta: [{ title: "Choose your device · Kit" }] }),
  component: DevicePicker,
});

const deviceItems = firmwareCatalog.map((device) => ({ label: device.name, value: device.id }));

function DevicePicker() {
  const search = Route.useSearch();
  const [deviceId, setDeviceId] = useState(
    findFirmwareDevice(search.device || "")?.id || DEFAULT_DEVICE_ID,
  );
  const device = findFirmwareDevice(deviceId)!;
  return (
    <KitChrome>
      <main className="flex flex-1 items-center justify-center p-6">
        <section className="flex w-full max-w-md flex-col gap-6">
          <header className="flex items-center gap-3">
            <IterateLogo className="size-9" />
            <h1 className="text-xl font-semibold tracking-tight">Set up your device</h1>
          </header>
          <p className="text-sm leading-relaxed text-muted-foreground">
            Choose your device, then the mind it works for.
          </p>
          <Field>
            <FieldLabel htmlFor="device">Device</FieldLabel>
            <FieldContent>
              <Select
                items={deviceItems}
                value={device.id}
                onValueChange={(value) => {
                  if (value) setDeviceId(value);
                }}
              >
                <SelectTrigger id="device" className="w-full">
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  <SelectGroup>
                    {deviceItems.map((item) => (
                      <SelectItem key={item.value} value={item.value}>
                        {item.label}
                      </SelectItem>
                    ))}
                  </SelectGroup>
                </SelectContent>
              </Select>
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
          <p className="text-sm leading-relaxed text-muted-foreground">
            Each device has its own access to the mind you choose. You can revoke its access at any
            time.
          </p>
          <form
            method="get"
            action={`/devices/${device.id}/mind`}
            className="flex flex-col gap-2 rounded-lg border p-4"
          >
            <FieldLabel htmlFor="mind-address">Set it up for a mind</FieldLabel>
            <div className="flex gap-2">
              <Input
                id="mind-address"
                name="address"
                placeholder="example.com"
                autoComplete="url"
                required
              />
              <Button type="submit" variant="outline">
                Continue
              </Button>
            </div>
            <FieldDescription>
              A mind is iterate at its own domain: type it, then sign in there and allow the device.
            </FieldDescription>
          </form>
        </section>
      </main>
    </KitChrome>
  );
}
