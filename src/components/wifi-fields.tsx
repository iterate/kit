import type { ComponentProps } from "react";
import { skipToken, useQuery, useQueryClient } from "@tanstack/react-query";
import { EyeIcon, EyeOffIcon } from "lucide-react";
import { Field, FieldContent, FieldDescription, FieldLabel } from "#/components/ui/field.tsx";
import { Input } from "#/components/ui/input.tsx";
import {
  InputGroup,
  InputGroupAddon,
  InputGroupButton,
  InputGroupInput,
} from "#/components/ui/input-group.tsx";
import {
  Select,
  SelectContent,
  SelectGroup,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "#/components/ui/select.tsx";
import { isStatusVoice, statusVoices, type StatusVoice } from "../firmware/config-image.ts";

export const horizontalFieldClassName =
  "grid gap-2 sm:grid-cols-[8.5rem_minmax(0,1fr)] sm:items-start sm:gap-4";

/** The board's Wi-Fi and how it says its status: the same on every setup page. */
export function WifiFields() {
  return (
    <>
      <Field className={horizontalFieldClassName}>
        <FieldLabel htmlFor="wifi-ssid" className="sm:pt-2">
          Wi-Fi network
        </FieldLabel>
        <FieldContent>
          <Input
            id="wifi-ssid"
            name="wifi-ssid"
            placeholder="Network name (SSID)"
            autoComplete="username"
            required
          />
          <FieldDescription>2.4 GHz only: the boards can’t join 5 GHz networks.</FieldDescription>
        </FieldContent>
      </Field>

      <Field className={horizontalFieldClassName}>
        <FieldLabel htmlFor="wifi-password" className="sm:pt-2">
          Wi-Fi password
        </FieldLabel>
        <FieldContent>
          <PasswordInput id="wifi-password" name="wifi-password" autoComplete="current-password" />
          <FieldDescription>Leave empty for an open network.</FieldDescription>
        </FieldContent>
      </Field>

      <Field className={horizontalFieldClassName}>
        <FieldLabel htmlFor="status-voice" className="sm:pt-2">
          Status voice
        </FieldLabel>
        <FieldContent>
          <Select name="status-voice" items={statusVoices} defaultValue="spoken">
            <SelectTrigger id="status-voice" className="w-full">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectGroup>
                {statusVoices.map((voice) => (
                  <SelectItem key={voice.value} value={voice.value}>
                    {voice.label}
                  </SelectItem>
                ))}
              </SelectGroup>
            </SelectContent>
          </Select>
          <FieldDescription>
            How the board says “Connecting to Wi-Fi”, “Ready” and “Call ended”. A board with a wake
            word also answers it with “Hello”.
          </FieldDescription>
        </FieldContent>
      </Field>
    </>
  );
}

/** Reads WifiFields from a submitted form, and offers the Wi-Fi to the browser's password manager. */
export function readWifiFields(form: FormData): {
  wifi: { ssid: string; password: string };
  statusVoice: StatusVoice;
} {
  const wifi = {
    ssid: String(form.get("wifi-ssid")),
    password: String(form.get("wifi-password")),
  };
  // Chrome offers to save it, then fills these fields next time (they're marked
  // username and current-password); an open network has nothing to keep
  if (wifi.password && "PasswordCredential" in window)
    navigator.credentials
      .store(new PasswordCredential({ id: wifi.ssid, password: wifi.password }))
      .catch((error: unknown) => console.warn("kit.wifi_not_saved", error));
  const statusVoice = String(form.get("status-voice"));
  if (!isStatusVoice(statusVoice)) throw new Error(`Unknown status voice ${statusVoice}`);
  return { wifi, statusVoice };
}

/** A password field with a show/hide button. Whether it shows lives in the query cache (like the
 *  wizard's flash progress), per field. */
function PasswordInput(props: ComponentProps<"input"> & { id: string }) {
  const queryClient = useQueryClient();
  const shownKey = ["kit", "password-shown", props.id];
  const shown = useQuery({ queryKey: shownKey, queryFn: skipToken, initialData: false }).data;
  return (
    <InputGroup>
      <InputGroupInput {...props} type={shown ? "text" : "password"} spellCheck={false} />
      <InputGroupAddon align="inline-end">
        <InputGroupButton
          size="icon-xs"
          aria-label="Show password"
          aria-pressed={shown}
          onClick={() => queryClient.setQueryData(shownKey, !shown)}
        >
          {shown ? <EyeOffIcon /> : <EyeIcon />}
        </InputGroupButton>
      </InputGroupAddon>
    </InputGroup>
  );
}
