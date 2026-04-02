# HostLink Build Breadcrumbs

_Resumption notes for the build agent. Updated at each milestone._

## Status: 🟡 STARTING

## GitHub
- Token: provided by user (not stored here — available in session context)
- Repo: to be created as `hostlink` under user's account

## Milestones

- [ ] 1. Repo created on GitHub
- [ ] 2. Project skeleton + build system in place
- [ ] 3. Protocol layer (protocol.c/h) + unit tests passing
- [ ] 4. Config parser (config.c/h) + unit tests passing
- [ ] 5. Logging + util layer
- [ ] 6. Daemon: event loop, accept, signal handling
- [ ] 7. Daemon: fork/exec, pipe capture, timeout
- [ ] 8. Client: connection layer, frame send/recv
- [ ] 9. Client: exec/ping/targets subcommands
- [ ] 10. Integration tests passing
- [ ] 11. systemd unit + install.sh
- [ ] 12. README.md complete
- [ ] 13. All tests green — notify @JebadiahJG on Telegram

## Notes

- Started: 2026-03-31 21:31 UTC
- Spec saved at: hostlink/SPEC.md
- User: @JebadiahJG on Telegram
