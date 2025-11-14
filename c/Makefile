all:
	@for proj in $$(ls -d */); do \
		cd $${proj} && make build && cd ..; \
	done
