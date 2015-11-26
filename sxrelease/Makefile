PRINT_INDENT = 16
RELEASE := ffmpeg-$(shell git describe master)

$(RELEASE).tar.xz: $(RELEASE).tar
	xz -k $^
	sha256sum $@
$(RELEASE).tar:
	cd .. && git archive --format=tar --prefix=$(RELEASE)/ master > $(PWD)/$@
patches:
	git format-patch n5.0 -o $@
print_dict: patches
	@(cd patches && for p in *; do printf "%*c{'name': '%s', 'level': 1},\n" $(PRINT_INDENT) " " $(basename $$p); done)
clean:
	$(RM) -r patches
	$(RM) *.tar *.tar.xz

.PHONY: upload print_dict replace_patches clean
