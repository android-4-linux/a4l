<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: original a4l documentation; Android's public documentation of Intent,
     intent filters and Custom Tabs. No GPL-3.0 sources. -->

# Web pages an app opens, and signing in through one

An app opens a web page with an `ACTION_VIEW` of an http or https address. Where the page
is shown depends on how the app asked:

| The app sends | The page is shown |
|---|---|
| a plain link | in the host's browser, through xdg-desktop-portal's `OpenURI` |
| a link that asks for a tab (a Custom Tabs session) | in the app's window |
| a plain link, on a host that opens nothing (a kiosk with no browser) | in the app's window |

A tab in the app's window is `org.acl.internal.BrowserActivity`: a bar with a close button
and the site's name, and the page under it in a WebView. It is on the app's Activity stack
like a screen of the app's own, and closing it, with the button or with Back, returns to the
screen that opened it.

`ACL_BROWSER` overrides the table:

| `ACL_BROWSER` | |
|---|---|
| unset | as above |
| `app` | every web link is shown in the app's window |
| `host` | every web link goes to the host's browser, tabs too |

A tab needs a WebView: `ACL_WEBVIEW_APK` names the WebView APK, and `tools/run-apk.sh` sets
it when it finds AOSP's prebuilt. With none, a link that asks for a tab goes to the host.

## Signing in

An app that signs in through a browser opens the sign-in page in a tab and waits. The site
ends by sending the browser to an address in a scheme of the app's own, such as
`myapp-auth://callback?code=…`, and the browser starts the Activity whose intent filter takes
that address. Only a browser inside the app can do that under a4l: the host's browser has no
way to start an Activity in a running app. That is why a tab is shown in the app's window.

When a page in a tab goes to an address that is not http or https, a4l looks for an Activity
of the app's that takes it from a browser: an exported Activity with an intent filter that
has `VIEW`, `DEFAULT`, `BROWSABLE` and matching `<data>`. It is started with the address,
over the tab. The app ends the tab the way it does on a phone, by bringing its own screen
back with `FLAG_ACTIVITY_CLEAR_TOP`. An address that no such Activity takes is not followed.

The same lookup applies when the app itself sends an `ACTION_VIEW` of an address in a scheme
of its own with no component named: the Activity whose filter takes it is started.

The log shows a sign-in as:

```
browser: opened https://login.example.com
browser: loaded https://login.example.com
browser: myapp-auth: address to com.example.app.RedirectActivity
```

Only the site is logged, never the path or the query, which carry the sign-in's answer.

## Limits

- The page runs in the app's process. It does not have the separation from the app that a
  tab in the user's browser has. Use `ACL_BROWSER=host` to keep pages out of the app, at the
  cost of sign-in.
- It shares nothing with the host's browser: no saved sign-ins, no password manager.
- A site that refuses embedded WebViews refuses this one. The WebView's user agent is left
  as it is.
- The page is drawn in software. `ACL_WEBVIEW_VULKAN` leaves it to a WebView built to draw
  through a4l's Vulkan functor.
- The page has no access to files or content providers, no location, no pop-up windows and
  no downloads.
- There is no Custom Tabs service for an app to bind to: no warm-up, no session callbacks,
  no styling of the bar.
- Typing needs a keyboard on the host. a4l has no on-screen keyboard.
