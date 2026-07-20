#! /bin/bash
#
# NOT FUNCTIONAL FOR THIS FORK.
#
# Everything below the version bump targets upstream infrastructure that mSL/NABI
# does not own: it pushes to github.com/linux-noah/noah and publishes a formula
# to the linux-noah/homebrew-noah tap. The names were deliberately NOT renamed
# to nabi along with the rest of the fork, because renaming them would only
# invent URLs that do not exist.
#
# Left in place as a record of the upstream release process. Replacing it with
# one that targets this fork is a separate piece of work.
#
VERSION=$1

# commit & push

FILE=`pwd`/`git rev-parse --show-cdup`/VERSION
echo "$VERSION" > $FILE
git add $FILE

while true; do
    read -p "commit and push? (type 'd' to show the changes) [y/n/d]" answer
    case $answer in
        "y") break;;
        "n") exit 1;;
        "d") git diff --cached
    esac
done

git commit -m "version $VERSION"
git push origin master

git tag $VERSION
git push origin --tags

echo successfully pushed noah $VERSION

# publish homebrew

URL=https://github.com/linux-noah/noah/archive/$VERSION.tar.gz

echo url: $URL

curl -LO $URL
SHA256=`shasum -a 256 $VERSION.tar.gz | cut -d ' ' -f 1`
rm $VERSION.tar.gz

echo sha256: $SHA256

if [[ -d homebrew-noah ]]; then
    cd homebrew-noah
    git reset --hard
    git pull
else
    git clone git@github.com:linux-noah/homebrew-noah.git
    cd homebrew-noah
fi

sed -i "" -e "4 s@.*@  url \"$URL\"@g" noah.rb
sed -i "" -e "5 s@.*@  sha256 \"$SHA256\"@g" noah.rb

git add noah.rb
git commit -m "version $VERSION"
git push origin master

cd ..
rm -rf homebrew-noah

echo successfully published noah $VERSION

while true; do
    read -p "upgrade noah on your system? [y/n]" answer
    case $answer in
        "y") break;;
        "n") exit 0;;
    esac
done

brew update
brew upgrade noah

echo successfully upgraded noah on your system
