# Contributing Guidelines

Thank you for considering contributing to this project! Please follow the guidelines below to ensure a smooth contribution process.

## Getting Started
1. **Fork the Repository**: Click the "Fork" button on the repository’s page.
2. **Clone Your Fork**: Run the following command:
   ```sh
   git clone https://github.com/<your-account>/greenhouse-Controller.git
   ```
3. **Enable the repository's git hooks** (once per clone). The pre-commit hook blocks a stamped
   web-asset manifest (gh#9), and checks the configuration-key descriptor on commits that touch it:
   ```sh
   git config core.hooksPath .githooks
   ```
4. **Create a Branch**: Use a descriptive branch name for your changes:
   ```sh
   git checkout -b feature/your-feature-name
   ```

## Making Changes
- Follow the project's coding style and guidelines.
- Keep commits focused and meaningful.
- Write clear commit messages following conventional commit format.
- Update documentation if applicable. A firmware change adds its section to `changelog.md`. Releases are
  built and published as described in [`bin/README.md`](bin/README.md).
- Before debugging anything odd, check [`memory/gotcha-log.md`](memory/gotcha-log.md): most surprises in
  this project have been met before.

## Submitting a Pull Request
1. **Push to Your Fork**:
   ```sh
   git push origin feature/your-feature-name
   ```
2. **Open a Pull Request**:
   - Navigate to the original repository.
   - Click on "New Pull Request".
   - Select your branch and provide a clear description of your changes.

## Code Review Process
- PRs will be reviewed by maintainers.
- Be open to feedback and make necessary changes.
- Keep your branch up to date by **rebasing** it onto `main` (`git rebase main`), never by merging
  `main` into it. Branch protection on `main` rejects merge commits, so a branch is integrated by
  rebase and fast-forward.

## Reporting Issues
- Check if the issue has already been reported.
- Provide detailed information, including steps to reproduce the issue.
- Use clear and concise language.

## Community Standards
- Follow the [Code of Conduct](code_of_conduct.md).
- Be respectful and collaborative.

Happy Coding! 🚀

