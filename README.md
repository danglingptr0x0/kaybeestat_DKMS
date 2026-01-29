## KayBee DKMS
Just want to say this right here in the beginning: this is a module for *enthusiasts*, not malicious actors.
Any key-logging, esp. at the kernel level, is understandably a huge, long-term security risk!
I added plenty of tests and made it such that it reveals outside the kernel as little possible, but the nature of it is still sensitive.
Hence, you should only really use this short-term when collecting data on your typing, which is also why I made it a DKMS module.
**DO NOT** use this in high-risk or high-visibility scenarios!
