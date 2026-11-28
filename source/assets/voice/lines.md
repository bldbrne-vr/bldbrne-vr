# Weapon voice lines

Each line plays once: the first time its weapon comes to the right hand, with the default motion
controls in first person (`src/core/vr/weapon_voice.h`). Only weapons with a non-standard
interaction have one: simple ones (a squeeze or a held grip to transform, a trigger's shot or
charge), left-hand weapons (guns, torches, shields) and the Hunter Axe (the user's choice) have
none. Switching the right hand's weapon stops a line. A line counts as heard when it plays to its
end, or after 3/4 of it. The lines run about 9 to 19 seconds with the reverb (the user: first "16s
is the upper limit", then "maybe 18-20s so it doesn't feel rushed").

**Recordings:** `make_recordings.py` writes each line dry into `dry/<key>.mp3`; `add_reverb.py` then
puts the same touch of stone-hall reverb on every line into `<key>.mp3` here, the file the game plays.
`launcher/package.ps1` ships the approved MP3s beside `shadps4.exe` in `runtime/voice/` and verifies
each recording in the full update package.

**Placeholders:** `make_placeholders.ps1 -Out <folder>` speaks these lines with Windows' own voices.
Write them outside this folder (next to a test build's `shadps4.exe`, in `voice/`), so a release
never ships them.

The lines describe the default controls: the right grip transforms, Original two-handed controls.
They say "your other hand", never left or right, so one recording also fits left-handed play.
The [bracketed] words are Eleven v3 audio tags (emotion, laughs, whispers): spoken as
performance, never as words; the placeholder voices skip them.

## Voices

Designed in ElevenLabs Voice Design as original voices (prompts below), or picked by the user from the
ElevenLabs Voice Library after their first takes "doesn't feel bloodborne" (2026-11-23). Never clone
the game's actors.

| voice | Voice Design prompt, or the Voice Library pick |
|---|---|
| Powder Kegs smith | Burly middle-aged man, deep gravelly voice, loud and cheerful, rough working-class English accent, a smith who loves explosives. |
| Old hunter | Voice Library: "Declan - Horror, Narrator" (old, raspy; the user's pick). |
| Brooding hunter | Voice Library: "Dominic - British, Brooding & Intense" (the user's pick). |
| Regretful nun | Native English, Southern British. Female, in her mid twenties. Studio quality. Persona: a devout young nun of a gothic cathedral. Emotion: sorrowful, guilt-ridden, tender. A soft, breathy, warm and alluring voice with a thick refined British accent; hushed and prayerful, slow and measured, as if confessing a sin she cannot take back. (Voice Design preview 2, the user's pick) |
| Church cleric | Calm, solemn middle-aged man, precise and cold, a soft cathedral echo. |
| Executioner | Voice Library: "Victor - Eloquent Evil Villain" (old, British; the user's pick). |
| Church assassin | Cold, hoarse whisper, slow and menacing, close to the ear. |
| Mad tinkerer | Voice Library: "Matthew Schmitz - Ancient Sage Dragon Wizard" (old, raspy, British; the user's pick). |
| Cainhurst noble | Voice Library: "Cate - Resonant, Deep and Elegant" (British; the user's pick). |
| Hushed scholar | Voice Library: "Nathaniel C - Calm, Suspenseful Narrator" (British, whispery; the user's pick). |
| Drowned fisherman | Voice Library: "Gideon - Pirate" (Irish, rough; the user's pick). |

## Lines

| key | voice | line |
|---|---|---|
| burial_blade | Old hunter | [quietly] The Burial Blade... a hunter's final kindness. Squeeze your grip, and take the staff in your other hand. It becomes a scythe. Reap them... then let go, and it is but a sword again. |
| blade_of_mercy | Regretful nun | [softly] The Blade of Mercy... Heaven forgive me, I have wielded it too. Squeeze your grip, take the second hilt, and pull them apart. [sighs] Rub them till they sing... and may your next cut be merciful. |
| rakuyo | Old hunter | [sighs] Rakuyo... a blade, and its sister. Squeeze your grip, take the other hilt, and pull them apart. Rub the edges together until they ring... and the next cut will not forgive. |
| simons_bowblade | Old hunter | [tired] A Bowblade... for those who would rather not get close. Hold your grip a moment, and it opens into a bow. Bring an arrow to the string, squeeze the trigger, draw... and loose. More arrows wait over your shoulder. |
| kirkhammer | Church cleric | [solemnly] The Kirkhammer. Squeeze your grip, then take the haft with your other hand, and the blade becomes the hammer. Release, and it is a sword once more. |
| ludwigs_holy_blade | Church cleric | [reverently] Ludwig's Holy Blade. Squeeze your grip and take the hilt with your other hand: a greatsword. Let one hand go, and it is a sword again. |
| rifle_spear | Church cleric | A Church Rifle Spear. Squeeze your grip and take the shaft in your other hand. Hold the trigger until it shudders three times, then let go. [coldly] The blast does the rest. |
| holy_moonlight_sword | Brooding hunter | [awed whisper] Ahh... the moonlight. Squeeze your grip, and it wakes within the blade. Hold the trigger until it trembles three times... then sweep the blade across your sight, and the light flies true. |
| logarius_wheel | Executioner | [fervently] The wheel of the Executioners! Squeeze your grip, and seize the far handle. Tap that hand's trigger... and the wheel turns, fiercer with every turn. [shouting] Grind the wretches down! |
| bloodletter | Church assassin | [whispers] Hold your grip and drive it into your own chest... [sinister chuckle] it wakes. Tap your other trigger, and the blood bursts. |
| tonitrus | Mad tinkerer | [excited] Tonitrus! Ha! Lightning, caught in a mace! Hold your grip, raise it high above your head, and it drinks the storm. [laughs] Then bring it down upon them! Give the sky a moment to rest. |
| chikage | Cainhurst noble | [coldly amused] The Chikage... a blade of Cainhurst, and a hungry one. Sheathe it at your hip, and it returns bloodied. Draw it swiftly, and you strike before they blink. Both hands upon it... and it feeds on you. |
| amygdalan_arm | Hushed scholar | [whispers] An arm of the Great Ones... still twitching. Squeeze your grip, and it uncurls. Swing, and it lashes. Hold the trigger mid-lash... [awed] and it seizes what it touches. Pull them close... or cast them off. |
| kos_parasite | Drowned fisherman | [gurgling] The parasite... it remembers the sea. With Milkweed in your blood, it lashes as you swing. Tap your other trigger, and it spits where you look. [whispers] Hold it... and the rite begins. |
| boom_hammer | Powder Kegs smith | [laughs] A bomb in a hammer, eh? Pump yer grip till she catches, then hold it, or smack her head with yer other hand. Ay, ready to blast! |
| whirligig_saw | Powder Kegs smith | [mischievously] Ahh, the Whirligig! Our finest work. Grab the long handle with yer other hand, and she opens into a wheel. Both hands on her, pull a trigger... [laughs] and listen to her sing! Beasts hate that sound. I love it! |
