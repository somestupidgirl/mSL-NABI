# Release Engineering

Run the following command (replace `0.2.3` with the version name you are going to publish with!):

    $ util/release-engineering.sh 0.2.3

The command includes the following actions...

- Update the version in the VERSION file
- Commit and push the changes
- Add a new tag and push it
- Publish the new version on the homebrew repository

A version name must consists of three natural numbers concatenated with two dots, like `0.2.3` or `1.3.11`.
