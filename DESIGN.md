# DESIGN.md

The visual language of the Droidspaces Android app. Every value here was read out of
`Android/app/src/main/java/com/droidspaces/app/`, not invented for this document. Where the app
disagreed with itself, the value most of the app already uses won.

The C backend has no UI, so none of this applies to `src/`.

## How to use this file

**Components come first, values second.** The Reuse Inventory in
[CONTRIBUTING.md](./CONTRIBUTING.md) lists what already exists. Go there first. A card is
`SettingsCard`, a dialog footer is `DialogFooterRow`, a status chip is `StatusPill`, an empty
list is `EmptyState`. If one of those fits, use it and stop reading here.

This file is for the case where you have checked and nothing shared covers what you need. Then
you build it out of the values below, so the new thing looks like it belongs.

Never hardcode a hex colour, an sp size, or a radius you picked by eye.

## What the app looks like, and why

**Flat, always.** Nothing in this app is elevated. `tonalElevation = 0.dp` appears at 55 sites
and there is not one `CardDefaults.cardElevation` in the tree. Depth is expressed by stepping
the surface colour up one level and drawing a 1dp border. If you reach for a shadow, you have
left the design.

**`Surface` is the primitive.** 123 `Surface` uses against a single `Card()`. Buttons are
clickable Surfaces, not Material `Button`s. Dialogs are `Dialog { Surface { } }`, not
`AlertDialog`. Material components are not banned, but they arrive with their own elevation and
radius defaults and have to be argued for.

**Colour is derived, never literal.** `ui/theme/Theme.kt` blends a full scheme from three
palette colours. Dynamic colour is on by default, six fallback palettes ship for older devices
and for users who turn it off, and AMOLED collapses every surface to black. A hardcoded
`Color(0xFF...)` breaks all four paths at once.

**Borders and tint carry state. Fills stay quiet.** A running container is not a green card. It
is a normal card with a tinted button and an accent border. State lives in the accent, not in
the background.

## Colour

Roles come from `MaterialTheme.colorScheme`. The scheme itself is built in `ui/theme/Theme.kt`,
which is the only file allowed to name a colour literally.

| Surface | Role |
| --- | --- |
| Screen background | `background` |
| Card sitting on the background | `surfaceContainer` |
| Card or pill nested inside another surface, and interactive settings rows | `surfaceContainerHigh` |
| Dialog and bottom sheet shell | `surfaceContainer` |

| Text | Role |
| --- | --- |
| Primary | `onSurface` |
| Secondary, supporting, metadata | `onSurfaceVariant` at alpha `0.7f` |
| Tertiary, deliberately quiet | `onSurfaceVariant` at alpha `0.6f` |
| Disabled content | `onSurface` at alpha `0.38f` |
| Disabled container fill | `onSurface` at alpha `0.12f` |

Borders are always 1dp. The alpha is what changes, and it is the value that has drifted the most
in this app, so it is worth getting right:

| Border | Colour |
| --- | --- |
| Card | `outlineVariant` at `0.35f` |
| Dialog shell | `outlineVariant` at `0.4f` |
| Action pill wrapper and other nested surfaces | `outlineVariant` at `0.2f` |
| Divider (`HorizontalDivider`) | `outlineVariant` at `0.3f` |
| Text field, unfocused | `outlineVariant` at `0.5f` |
| Text field, focused | `primary` at `0.8f` |
| Tinted button, matching its accent | the accent role at `0.2f` |

### What the colours mean

The container lifecycle is consistent across the app and new states should join it rather than
invent a parallel vocabulary.

| State | Accent |
| --- | --- |
| Running, healthy, start action | `primary` |
| Stopped, failed, destructive, stop action | `error` |
| Restart action | `secondary` |
| In progress, restarting, downloading | `tertiary` |
| Idle, unknown, nothing to report | `onSurfaceVariant` at `0.6f` |
| Disabled | fill `onSurface` at `0.08f`, accent `outlineVariant` |

A tinted button takes its fill from the matching container role at alpha `0.4f`, for example
`primaryContainer.copy(alpha = 0.4f)` for start and `errorContainer.copy(alpha = 0.4f)` for stop,
with a 1dp border of the accent at `0.2f`.

The init system screens are the one documented exception. Their status hues live in
`statusColorFor()` in `ui/screen/InitServiceScreen.kt` and are a hand picked traffic light, not
theme roles, because six of those states can sit in the legend row at once and `primary` and
`tertiary` move with dynamic colour and the palette picker. Take a status colour from that
function, never from a literal.

## Typography

The scale lives in `ui/theme/Type.kt` and all fifteen slots are configured. Use the named styles.
There are only twelve `fontSize` overrides in the whole app and none of them are a good example.

UI text is `FontFamily.Default`. Anything the machine produced, a log line, a unit name, a config
file, a path, is JetBrains Mono, imported from `ui/theme/Type.kt`. Do not declare a local font
family.

| Element | Style |
| --- | --- |
| Screen title in a `TopAppBar` | `titleLarge`, Bold |
| Dialog title | `titleLarge`, Bold, left aligned |
| Card title | `titleMedium`, SemiBold |
| Section header inside a screen | `titleSmall`, Bold, `primary` |
| Button label | `labelLarge`, SemiBold |
| Body copy in a dialog | `bodyMedium` |
| Helper text, metadata, subtitles | `bodySmall` with `onSurfaceVariant` at `0.7f` |
| Status pill label | `labelSmall`, Black, `letterSpacing = 0.5.sp`, uppercase |
| Machine output | `bodySmall` with the JetBrains Mono family |

## Spacing

The scale is **4, 8, 12, 16, 20, 24, 32**. Nothing else. If a gap feels like it wants 14, it
wants 12 or 16.

| Gap | Value |
| --- | --- |
| Screen edge, lists and dashboards | 16 horizontal |
| Screen edge, wizard and setup flows | 24 horizontal |
| Between cards in a list | 16 |
| Between blocks inside a card | 12 |
| Card inner padding | 16 |
| Dialog inner padding | 24 |
| Icon to its label, chip to chip | 8 |
| Tight metadata rows | 4 |

Screens that sit under the floating tab bar add `bottom = 120.dp` to their content padding so the
last card clears it.

### Card headers line up across tabs

The status card on the home tab, the container card, the running container card and the init
service row all put a title on the left and a status pill on the right, above a divider. A user
switching tabs sees those dividers and pills hold the same line, and that is deliberate.

It only holds while all four use the same numbers, so they come from `CardContentPadding` and
`CardHeaderHeight` in `ui/component/CardMetrics.kt` rather than being typed out per card. The
header is 48dp because the container card puts an icon button in it.

If you add a card with this shape, take the values from there. If you change them, you are
moving every card header in the app, which is the intended way round.

## Shape

| Element | Radius |
| --- | --- |
| Card | 20 |
| Dialog shell | 24 |
| Bottom sheet, top corners only | 24 |
| Full width primary button | 20 |
| Dropdown menu | 20 |
| Action button | 16 |
| Text field | 16 |
| Action pill wrapper | 12 |
| Status pill, small badge | 8 |

## The action pill

This is the pattern that keeps getting "fixed", so it gets its own section.

A row of related actions sits inside a tinted wrapper:

```kotlin
Surface(
    shape = RoundedCornerShape(12.dp),                    // wrapper
    color = MaterialTheme.colorScheme.surfaceContainerHigh,
    border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.2f))
) {
    Row(Modifier.padding(4.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        Surface(
            modifier = Modifier.weight(1f).height(48.dp),
            shape = RoundedCornerShape(16.dp),            // button, larger than the wrapper
            color = accentContainer.copy(alpha = 0.4f),
            border = BorderStroke(1.dp, accent.copy(alpha = 0.2f))
        ) { /* icon + labelLarge SemiBold */ }
    }
}
```

**The buttons are rounder than the box they sit in. That is the design.** The 4dp inset plus the
larger inner radius makes the buttons read as sitting proud of the wrapper rather than nesting
flush inside it. Matching the two radii flattens the effect and has been reverted twice already.

Two sites use it: `ui/component/ContainerCard.kt` and `ui/screen/InitServiceScreen.kt`. It is for
a row of peer actions inside a card. A dialog's confirm and cancel are not that, and have their
own rule below.

## Dialogs

Every dialog is `DsDialog`, and it owns the layout, not just the frame:

```
Surface(fillMaxWidth, padding 24, heightIn(max = screen - 48))   bounded by LocalConfiguration
  Column(padding 24, spacedBy 16)
    Column(weight(1f, fill = false), scrolls)   the body: shrinks, scrolls
    footer()                                    natural height, measured first
```

The bound comes from `LocalConfiguration`, not the window. The activity handles
`configChanges` itself, so a dialog open through a rotation keeps its stale window
constraints; the configuration is live, and it is what `TerminalDialog` has always sized
from.

**Actions go in the `footer` slot, never in the content.** That is the whole point of the
component. A `Column` measures its unweighted children in order against the space that is left,
so actions placed below a scrolling body get measured with whatever remains: fine in portrait,
a sliver or nothing in landscape. The footer is unweighted and declared second, so it is
measured first and always gets its full height.

Callers therefore do not set a width, their own padding, or their own scroll, and never pass
`wrapContentHeight()`, the shell already wraps. What is genuinely per-dialog rides on the
`modifier`: `imePadding()` for a text field, and for a dialog that should stay modest, a cap on
the whole dialog, `heightIn(max = ...)` or a `fillMaxHeight` fraction, never on an inner list.
A shell-level cap is safe because a squeezed body scrolls instead of eating the footer.
Destructive dialogs pass `borderColor` to outline in `error`. A body built on a `LazyColumn`
passes `scrollableContent = false` and weights the list, `fill = false` if the dialog should
wrap when the list is short, because a lazy list cannot live inside a scrolling parent.

The title is `titleLarge` Bold, left aligned, and is the first thing in the content.

## Dialog actions

Every dialog ends with `DialogFooterRow`. It is two equal-weight buttons, a quiet tonal dismiss
and a filled confirm:

| | Value |
| --- | --- |
| Row | `fillMaxWidth`, `spacedBy(12.dp)` |
| Both buttons | `weight(1f).height(48.dp)` |
| Radius | 16 |
| Dismiss | fill `onSurface @ 0.06f`, border `1.dp outlineVariant @ 0.35f`, label `onSurfaceVariant` |
| Confirm | fill `primary`, label `onPrimary`, no border |
| Confirm, destructive | fill `error`, label `onError`, via `destructive = true` |
| Confirm, disabled | fill `onSurface @ 0.12f`, label `onSurface @ 0.38f` |

Equal weight and a fixed height are the point: the two buttons are always the same size, and
neither one changes because of the other's label.

**Labels are one line and ellipsize.** If a label wraps, the label is too long. Shorten the
string, do not grow the button. "Allow" and "Not now" beat "Grant Permission" and "I Understand"
on a control that is half a dialog wide.

A dialog with a single action uses the dismiss button on its own, full width. The footer always
draws a pair.

## Icons and touch targets

| Use | Size |
| --- | --- |
| Leading icon on a card or list row | 20 |
| Toolbar action, icon inside a button | 18 |
| Inline with `bodySmall` text | 16 |
| Dialog header, distro identity | 24 |
| Empty state hero | 64 |
| Status dot | 6 |

Anything tappable is at least **48dp**. That is an accessibility floor, not a style preference,
and it is the one rule in this file that a design argument does not get to override.

## Alignment

Rows are `Alignment.CenterVertically`. The app does this 101 times and deviates once.

Card titles and dialog titles are left aligned, always. A card row that pairs a title with a
control uses `Arrangement.SpaceBetween`.

Centred text is reserved for full screen states: empty lists, loading, setup heroes. If content
is centred inside a card, it is probably wrong.

## Motion and loading

Spinners come from `LoadingIndicator` with a `LoadingSize`, never a raw `.size(n.dp)` on a
progress indicator. Whole screen loading is `FullScreenLoading`. Animation timings come from
`AnimationUtils`.

## When the rule does not fit

Sometimes it will not, and the answer is not to quietly use a different number.

Say it in the PR, and leave a comment at the site explaining what the deviation buys, the way the
action pill wrappers now do. An undocumented deviation is drift and the next contributor will
"fix" it. A documented one is a decision, and it survives.

## Decided exceptions

Looked at, deliberately left alone. Do not re-open these without a reason the original one
misses.

- **`TerminalDialog` and `ProgressDialog` do not use `DsDialog`.** The first is three quarters
  of the screen with its own header row, the second is not dismissible. If a third dialog ever
  wants either shape, that is the moment to widen the shared shell.
- **The init system status hues** (`statusColorFor()` in `ui/screen/InitServiceScreen.kt`) stay
  a hand picked traffic light rather than theme roles. Six of the seven states can sit in the
  legend row at once, and `primary` and `tertiary` move with dynamic colour, so running and
  abnormal would collapse onto nearly the same colour under some palettes.
- **The terminal virtual keys background** (`ui/screen/ContainerTerminalScreen.kt`) stays a
  literal. It sits against the terminal's own black, which does not follow the app theme, so a
  surface role would give a light strip under a dark terminal.
- **The root check button's disabled colours** (`ui/screen/RootCheckScreen.kt`) stay equal to
  its enabled colours. It is disabled only while a check is in flight, and greying it out for
  that moment reads as a flicker.
- **The unit detail and override editor titles** keep a smaller style than every other screen
  title. Both display systemd unit names, which are long, so `titleLarge` would only ellipsize
  sooner.
- **`EmptyState` is not adopted** by `ui/screen/AutoBootPriorityScreen.kt` or the private empty
  state in `ui/screen/InitServiceScreen.kt`: absorbing them needs roughly seven new parameters
  for two call sites. `ui/component/ContainerUsersCard.kt` is not an empty state at all, just a
  plain `Text` in a card body.
- **Faint fill alphas of 0.03, 0.04 and 0.06** in the terminal dialog, the terminal screen and
  the sparse image screen stay unshared. They are invisible in isolation, and snapping them
  together would mean inventing a token for something nobody can see.
