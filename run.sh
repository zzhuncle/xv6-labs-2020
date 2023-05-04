wget http://kr.archive.ubuntu.com/ubuntu/pool/main/b/brltty/libbrlapi0.7_6.0+dfsg-4ubuntu6_amd64.deb
sudo dpkg -i libbrlapi0.7_6.0+dfsg-4ubuntu6_amd64.deb

wget http://security.ubuntu.com/ubuntu/pool/main/n/nettle/libnettle7_3.5.1+really3.5.1-2ubuntu0.2_amd64.deb
sudo dpkg -i libnettle7_3.5.1+really3.5.1-2ubuntu0.2_amd64.deb

wget http://archive.ubuntu.com/ubuntu/pool/main/q/qemu/qemu-system-misc_4.2-3ubuntu6_amd64.deb
sudo dpkg -i qemu-system-misc_4.2-3ubuntu6_amd64.deb

git reflog # 查看每一次命令

git reset --hard HEAD^ # 回滚到上个版本

git branch -a # 查看分支

riscv64-unknown-elf-gdb # 调试