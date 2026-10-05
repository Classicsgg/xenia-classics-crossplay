Xenia Classics Crossplay (crossplay build 8)
============================================

A modified Xenia Canary (netplay) that joins Call of Duty 4 and Modern
Warfare 2 matches hosted on a real Xbox 360, with Xbox 360 and PS3 players.
Your game traffic goes to the host's relay over the internet; the 360 sees
you as a normal player.

Full setup guide:  https://classics.gg/ranked/how-it-works  (PC - Xenia)
Discord:           https://classics.gg/discord
Source code:       https://github.com/Classicsgg/xenia-classics-crossplay

NOT INCLUDED: the games. Use your own Xbox 360 copies, on the same title
update as the host:
  Call of Duty 4           Title Update 4
  Modern Warfare 2         Title Update 9


What's in here
--------------
  xenia_canary_netplay.exe          the crossplay build
  xenia-canary-netplay.config.toml  settings (already set up for crossplay)
  crossplay_bindings.ini            keyboard and mouse bindings (KBM mode)
  portable.txt                      keeps Xenia's settings/profiles in this folder
  Default.mpdata                    CoD4 custom classes loaded when you join
  xstorage\title\41560817\playlists.patch2
                                    MW2 playlists (lets you join the host's
                                    Xbox Live private match). Keep it at
                                    exactly this path next to the exe.
  LICENSE                           Xenia's license


Classics Ranked: link your Discord (once)
-----------------------------------------
PC ranked needs the Subscriber role in the Classics Discord. It counters
cheating, which is easier on PC, and helps pay for server hosting. $2 from
every server subscription goes into the ranked season's prize pool.

The first time you start Xenia, a box at the top of the screen shows a code:

    In the Classics Discord, type:   /link 7F3KQ9

Type /link and that code in the Discord (#link-xenia works). Only you see the
reply. The box closes by itself within a few seconds: you're linked, and
Xenia saves the link in its config. Your ranked profile belongs to your
Discord account, whatever Xenia profile you use. /linkstatus in the Discord
shows your link.

If you link while a game is already running, restart the game so it plays
under your linked identity.

Played ranked on Xenia before linking? Tell an admin your gamertag after you
link, so your rating moves to your linked account.


Setup (once)
------------
1. Unzip this folder somewhere, e.g. C:\ClassicsCrossplay\  (not inside
   Program Files).

2. Settings live in xenia-canary-netplay.config.toml, [Live] section. They're
   already set; only change them if the host tells you to. CLOSE Xenia
   before editing (it rewrites the file when it exits).

       bridge_address = "classicsgg.duckdns.org:31000"
       bridge_enabled = true
       bridge_key     = "classics"
       bridge_fake_live = true          (MW2 Xbox Live private matches)
       bridge_mw2_unlock_all = true     (MW2 custom classes, see below)
       bridge_stats_file = "Default.mpdata"    (CoD4)
       bridge_input_mode = "controller" (or "kbm" for keyboard and mouse)

   Leave bridge_install_id, bridge_token and bridge_link_xuid alone: Xenia
   fills them in when you link. Don't share them; they are your link.

   CoD4 only: optionally name your five custom classes:
       bridge_class_names = "AR,SMG,Sniper,Shotgun,LMG"

3. Allow Xenia through Windows Firewall when Windows asks (private AND public).

4. Start Xenia, create a profile and sign into it. Every player needs their
   OWN profile. TICK "Xbox Live Enabled" when you create it. Already have a
   profile? In the profile menu, right-click it -> "Convert to Xbox
   Live-Enabled Profile". (Nothing connects to real Xbox Live -- MW2 just
   needs a Live profile to join the host's private match party.)


Joining a match
---------------
1. Open xenia_canary_netplay.exe and load the game's default_mp.xex
   (File > Open > your MW2 or CoD4 folder > default_mp.xex).
2. Within a few seconds a notice appears at the top of the screen:
     "Crossplay relay connected - The 360 host is online. Open System Link
      to find the match."                   -> go to step 3
     "...the 360 host isn't running a match yet."
                                            -> wait; a new notice appears
     "Crossplay: no answer from relay"      -> see "If something goes wrong"
     "Crossplay: can't join" + a reason     -> see "If something goes wrong"
3. Multiplayer -> System Link -> Find Game. Join the Classicsgg lobby.
   This works whether the host is in a System Link game or an Xbox Live
   private match -- you always join from System Link.


Modern Warfare 2: Xbox Live private matches (host's party)
----------------------------------------------------------
Join Game puts you IN the host's party, like an invited 360 player:
  - Host in the pregame lobby: you join the lobby and load in with everyone.
  - Host already in a match: you drop straight into it.
  - At the end of a match you go back to the lobby with everyone and follow
    the host into the next match (ranked matches included).
Opening Xbox Live from MW2's own main menu does NOT work in Xenia -- always
join from System Link.


Modern Warfare 2: custom classes
--------------------------------
A few seconds after you open System Link, your profile's System Link (offline)
progress is raised to level 70 with everything unlocked, once. Build your
classes BEFORE joining: System Link -> Create a Class -> edit -> back out.
Only offline progress is changed. To turn it off: bridge_mw2_unlock_all = false.


Keyboard and mouse
------------------
Set bridge_input_mode = "kbm" (Xenia closed). KBM removes the game's aim
assist and the host is told you're on KBM. A lobby can be controller-only;
then you'll see "can't join" with the reason. Edit crossplay_bindings.ini to
change keys.


If something goes wrong
-----------------------
- "Crossplay: no answer from relay": check bridge_address (edit with Xenia
  CLOSED), then ask in the Discord whether the relay is up.
- "Crossplay: can't join" -- the notice says why:
    key      your bridge_key doesn't match the host's
    title    you're running a different game than the host
    full     the host's crossplay slots are full -- try again later
    input    this lobby is controller only, or KBM without aim assist removed
- "Crossplay: update needed": get the latest release.
- Link box says you can't play ranked from this PC:
    "Subscriber role required"      subscribe in the Discord, then restart Xenia
    "not in the Classics Discord"   join the Discord, then restart Xenia
    "already in use on another PC"  your link is playing on another PC right
                                    now; one link plays on one PC at a time
    "Restart the game"              you linked while the game was running
- MW2, "Game lobby closed" / "Timed out" when joining a private match: check
  the profile is Xbox Live-Enabled, bridge_fake_live = true, and
  playlists.patch2 is in xstorage\title\41560817\ next to the exe.
- MW2 can close after a map change ("Title was restarted..."). Restart Xenia,
  load MW2 again and rejoin from System Link.
- Ask in the Discord and include xenia.log (in this folder).
