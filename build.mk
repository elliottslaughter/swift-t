
# BUILD.MK
# Use via build.sh

ASCIIDOC = asciidoc --attribute stylesheet=$(PWD)/swift.css \
                    -a max-width=800px

# Must compile leaf.txt with make-stc-docs.zsh (snippets, etc.)
all: guide.html gallery.html dev.html sites.html

define ASCIIDOC_CMDS
	@ echo ASCIIDOC $(<)
	@ $(ASCIIDOC) $(<)
endef

%.html: %.txt
	$(ADOC_CMDS)

guide.html: guide.txt
	$(ADOC_CMDS)
	./google-analytics.sh $(@)

pubs.html: swift-t.list.adoc

# Gallery has extra dependencies and uses M4 to assemble
GALLERY_SWIFT = $(shell find gallery -name "*.swift")
GALLERY_SH    = $(shell find gallery -name "*.sh")
GALLERY_CODE = $(GALLERY_SWIFT) $(GALLERY_SH)

# This file is an intermediate artifact
gallery.txt: code.m4 gallery.txt.m4
	@ echo M4 $(<)
	@ m4 $(^) > $(@)

gallery.html: gallery.txt $(GALLERY_CODE)
	@ echo ASCIIDOC $(<)
	@ $(ASCIIDOC) $(<)

clean:
	rm -fv gallery.txt
	rm -fv leaf.html swift.html
	rm -fv leaf__1.*
