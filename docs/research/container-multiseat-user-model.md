# Container multiseat: the user model

Companion to `container-multiseat-architecture.md`. That document settles how the
host allocates a seat. This one settles what a seat is to the person using it,
because the two are not the same question and only the first has been answered.

## Why this is written before the user interface

The encoder provider is the next piece of the arc to be built, and it is built
against a contract that assumes a seat's identity and its resource reservation.
Whatever the person on the client is choosing has to be expressible in that
contract. Getting it wrong is not a screen to redraw, it is a contract to
rebuild, so it is cheaper to settle now than after.

Nothing here proposes building a user interface yet. Multiseat cannot stream:
there is no encoder provider, no streaming-capable worker image, and no
activation key. A screen for any of that would be a screen for nothing.

## Two nouns, not one

Today the vocabulary has one word, "seat", doing two jobs. The architecture
already forces them apart.

**A profile is what someone owns.** It persists. The storage model lists what
lives in it: credentials and tokens, the Steam, Heroic and Lutris databases,
Wine and Proton prefixes, shader and config writes, and save data. It survives
every session and outlives every container.

**A seat is what someone is using right now.** It is ephemeral: a slot on a
logical GPU with an encoder session reserved against it, a compositor, an audio
endpoint, and a set of verified input nodes. It runs from `reserved` to
`released`, and its identity is deliberately opaque, being a controller epoch
and a generation rather than anything about the person.

The binding rule between them is already decided, in the storage model rather
than as a product choice: two active workers must never mount one mutable
launcher home read-write, so the registry rejects a second active seat for the
same profile even from a different client. Stated for a person:

> **A profile can be in use in one place at a time.**

That is worth keeping because it is honest about the underlying constraint and
because Steam enforces the same thing, so it will surprise nobody.

## How a client gets a profile

This is the open question, and the one that needs deciding.

**Recommended: a profile is a property of the pairing, one to one by default,
reassignable by the operator.**

- A newly paired client gets its own profile. The common case, one person with
  one device, needs no configuration and behaves exactly as today.
- The operator may point several paired clients at one profile, so a handheld
  and a television share one library and one set of saves, accepting that only
  one of them can stream at a time.
- The operator may create a profile that no client owns yet, which is what makes
  a guest or a spare identity possible.

Two alternatives were considered and rejected.

*Per application* does not work because an application is not an identity. Steam
credentials and Proton prefixes are not per game, and the storage model is
already organised around the account rather than the title.

*Fully manual, choose a profile at every launch* was rejected because it makes
the simplest case pay setup cost before the first stream, and because the
one-active-seat rule means the choice is usually forced anyway.

## What the host operator configures

Deliberately small.

- **Per logical GPU: how many seats, and how many simultaneous encoder
  sessions.** These are already two independent budgets in the architecture, and
  an unknown or zero budget must not be read as unlimited.
- **Profiles: a name, where their storage lives, and which paired clients may
  claim them.**

Nothing else. Compositor selection, GPU placement and slot numbers stay
automatic, and the architecture is explicit that a seat receives one final
render and encode allocation rather than rediscovering devices inside the
container.

## What a client needs to be told

Before launch, which profile it will use and whether that profile is already in
use somewhere else.

On refusal, **which of three distinct reasons applies**, because they have
different fixes and must not collapse into "host unavailable":

| Refusal | What the person can do |
|---|---|
| The profile is already streaming elsewhere | Stop the other session, or use a different profile |
| No free seat on any GPU | Wait, or raise the seat budget |
| No free encoder session | Wait, or raise the encoder budget |

## What this constrains in the work being built now

1. **The encoder provider accepts its reservation as given.** It does not choose
   or discover a device. That follows from the architecture's allocation rule and
   from the encoder-session budget existing separately from the seat budget.
2. **The activation key is not a boolean over behaviour.** Enabling multiseat
   with no profiles configured must be a no-op that leaves single-seat streaming
   exactly as it is, not an error and not a behaviour change. This is why the key
   comes last: a key that starts seats which announce nothing produces sessions
   that end themselves.
3. **Admission refusal needs three distinct reasons on the wire**, before any
   client can show them.
4. **The worker still never learns the profile name.** Client and profile keys
   stay internal routing metadata and never appear in worker, runtime, Wayland,
   audio or input resource names. The user model adds a concept above that
   boundary; it does not move the boundary.

## Deliberately not decided here

- Whether profiles map onto real Linux user accounts. The storage model needs
  isolation, not necessarily separate accounts, and the answer changes the
  container boundary rather than the user model.
- Save synchronisation, which the storage model already defers to a separate
  contract.
- What happens to a running seat when its profile is reassigned. Refusing the
  reassignment while streaming is the obvious answer and needs confirming
  against the lifecycle rules rather than asserted here.
- Guests and temporary profiles beyond noting that unowned profiles are what
  make them possible.
