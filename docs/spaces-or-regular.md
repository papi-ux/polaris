# Spaces or regular streaming?

Both run on the same Polaris host, and a paired device can use either. The
difference is what a session owns.

Regular streaming plays the games installed on your PC, signed in as you, and
Polaris picks where the session runs: your desktop, a private compositor, or a
display it creates for the stream. A Space is a separate Steam sign-in, game
library and set of saves on the same PC, kept in its own container, chosen per
device. Spaces are a preview; the limits in the table are the ones that shape
the choice today.

| | Regular streaming | Spaces (preview) |
| --- | --- | --- |
| Who it is for | One player, or one shared Steam account | Each player, or each device, with their own sign-in and saves |
| Sign-ins and saves | Your PC's Steam and its saves | One Steam per Space; installed games, saves and the sign-in stay in the Space |
| What runs it | The games installed on the host | The Polaris gaming runtime image, with Steam inside; the host needs no Steam |
| Host requirements | Polaris and a GPU driver | Polaris, Docker Engine, the runtime image, a GPU seat, Linux only |
| Session modes | Mirror Desktop, Private Stream, Host Virtual Display, Desktop Takeover, Gamescope Stream | Gamescope Stream inside the Space |
| HDR | Where the capture path carries it | Not in the preview |
| Audio | Host audio | Under investigation on handhelds in the preview |
| How many at once | One session on the host | One Space at a time in the preview |
| In Nova | The Library | The Library for the chosen Space, with Change Space, statuses and reasons |
| Setup | Install and pair | Host Setup on the Spaces page, seven checks, then the first Space |
| When it breaks | Doctor and the launch's own reason | The same reasons, and the Spaces page says why a Space cannot be offered |

## Both on one host

Nothing about Spaces changes regular streaming. A device with no Space assigned
keeps the Library it has today, and a device with a Space can be sent back to
the Desktop from the Spaces page. Use regular streaming for the modes a Space
does not offer, such as Mirror Desktop and HDR, and a Space when two people need
their own Steam on one PC, or when a handheld and a TV share one sign-in at
different times.

## Read next

- [Spaces](spaces.md): what you need, Host Setup, the first Space, devices, what
  disconnect does, and what to do when it breaks.
- [Launch modes](launch-modes.md): what each regular mode does to your display.
- [Runtime](runtime.md): the private compositor and the displays Polaris creates.
