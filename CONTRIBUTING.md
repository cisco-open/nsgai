<!--
SPDX-License-Identifier: Apache-2.0

Copyright (c) 2026 by Cisco Systems, Inc., and/or its affiliates.
All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

	http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# How to Contribute

Thanks for your interest in contributing to nsgai. These guidelines explain
how to report issues and submit changes.

All interactions in this project are subject to our [Code of Conduct](CODE_OF_CONDUCT.md).

## Reporting Issues

Before reporting a new issue, please check whether it has already been
reported or fixed by searching the [issue tracker](https://github.com/cisco-open/nsgai/issues).

When creating an issue, include a clear title and description, relevant
versions and environment details, steps to reproduce the problem, and a test
case or logs where possible.

**Please do not report security vulnerabilities through a public issue.** See
[SECURITY.md](SECURITY.md) for the security reporting process.

## Sending Pull Requests

Before opening a pull request, review existing issues and pull requests to
see whether the proposed change has already been discussed or implemented.

Pull requests should:

* Explain the motivation and scope of the change.
* Include tests for affected behavior.
* Update documentation when appropriate.
* Preserve the project's C11, CMake, and formatting conventions.
* Pass the relevant build and test checks described in `README.md`.

We follow semantic versioning and may defer breaking changes until the next
major version release.

## Other Ways to Contribute

You can also help by triaging issues, reviewing pull requests, testing patches
against applications that use nsgai, improving documentation, or adding
regression tests.

Thank you for contributing to nsgai.
