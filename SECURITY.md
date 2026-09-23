# Security Policies and Procedures

This document describes how to report and manage security vulnerabilities in
nsgai.

## Disclosing a Security Issue

The nsgai maintainers take security issues seriously. Please do not disclose
a suspected vulnerability in a public GitHub issue, pull request, or
discussion.

nsgai uses GitHub's private vulnerability reporting. To submit a report,
use the [private vulnerability reporting page](https://github.com/cisco-open/nsgai/security/advisories/new).

Please include, when available:

* A detailed description of the issue.
* The steps required to reproduce it.
* Versions of nsgai that may be affected.
* Any known mitigations or workarounds.
* Whether the issue may involve the c-ares dependency or a downstream build.

A maintainer will acknowledge the report within three business days and will
send a more detailed response within an additional three business days
indicating the next steps. If you cannot successfully submit a private report
through GitHub, contact the Cisco Open Source security team at
[oss-security@cisco.com](mailto:oss-security@cisco.com).

After the initial reply, the maintainers will endeavor to keep you informed of
progress toward a fix and public announcement, and may ask for additional
information or guidance.

## Vulnerability Management

When the maintainers receive a disclosure report, they will assign it to a
primary handler. The handler will coordinate the fix and release process,
which includes:

* Confirming the issue.
* Determining affected versions of nsgai and relevant dependency versions.
* Auditing the code for similar issues.
* Preparing fixes for supported releases.
* Coordinating disclosure and release timing with the reporter.

## Dependency Vulnerabilities

nsgai dynamically depends on c-ares but does not vendor it in source
releases. The impact of a c-ares vulnerability depends on the version selected
by the downstream builder or operating system. We will document the impact on
nsgai releases when applicable and recommend fixed c-ares versions.

## Suggesting Changes

Suggestions for improving this process may be submitted as a pull request.
Please use private reporting for any suggestion that contains vulnerability
details.
