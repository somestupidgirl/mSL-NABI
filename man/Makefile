# Use the remark-man node-js package to generate the documentation off the
# GitHub markdown file.
# See https://github.com/remarkjs/remark-man.
nabi.1: nabi.md
	node nabi.js

nabi.ps: nabi.1
	groff -tmandoc nabi.1 > nabi.ps

clean:
	rm -f nabi.ps
