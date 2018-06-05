# NOTE: link the curent working copy of the code to elmon.c for compiling
CFLAGS=-g -O2 -D JFS -D GETUSER -Wall -D LARGEMEM
# CFLAGS=-g -O2 -D JFS -D GETUSER -Wall -D POWER
#CFLAGS=-g -D JFS -D GETUSER 
LDFLAGS=-lncurses -g
FILE=elmon.c

elmon_power_rhel3: $(FILE)
	cc -o elmon_power_rhel3 $(FILE) $(CFLAGS) $(LDFLAGS) -D POWER

elmon_power_rhel4: $(FILE)
	gcc -o elmon_power_rhel4 $(FILE) $(CFLAGS) $(LDFLAGS) -D POWER

elmon_power_sles11: $(FILE)
	cc -o elmon_power_sles11 $(FILE) $(CFLAGS) $(LDFLAGS) -D POWER

elmon_power_sles10: $(FILE)
	cc -o elmon_power_sles10 $(FILE) $(CFLAGS) $(LDFLAGS) -D POWER

elmon_power_rhel5: $(FILE)
	gcc -o elmon_power_rhel5 $(FILE) $(CFLAGS) $(LDFLAGS) -D POWER

elmon_power_sles9: $(FILE)
	cc -o elmon_power_sles9 $(FILE) $(CFLAGS) $(LDFLAGS) -D POWER

elmon_power_sles8: $(FILE)
	cc -o elmon_power_sles8 $(FILE) $(CFLAGS) $(LDFLAGS) -D POWER

elmon_mainframe_sles8: $(FILE)
	cc -o elmon_mainframe_sles8 $(FILE) $(CFLAGS) $(LDFLAGS) -D MAINFRAME

elmon_mainframe_sles9: $(FILE)
	cc -o elmon_mainframe_sles9 $(FILE) $(CFLAGS) $(LDFLAGS) -D MAINFRAME

elmon_mainframe_sles10: $(FILE)
	cc -o elmon_mainframe_sles10 $(FILE) $(CFLAGS) $(LDFLAGS) -D MAINFRAME

elmon_x86_sles8:  $(FILE)
	cc -o elmon_x86_sles8 $(FILE) $(CFLAGS) $(LDFLAGS)

elmon_x86_sles10:  $(FILE)
	cc -o elmon_x86_sles10 $(FILE) $(CFLAGS) $(LDFLAGS)

elmon_x86_opensuse10:  $(FILE)
	cc -o elmon_x86_opensuse10 $(FILE) $(CFLAGS) $(LDFLAGS)

elmon_x86_64_opensuse11:  $(FILE)
	cc -o elmon_x86_64_opensuse11 $(FILE) $(CFLAGS) $(LDFLAGS)

elmon_x86_sles9:  $(FILE)
	cc -o elmon_x86_sles9 $(FILE) $(CFLAGS) $(LDFLAGS)

elmon_x86_rhel45:  $(FILE)
	cc -o elmon_x86_rhel45 $(FILE) $(CFLAGS) $(LDFLAGS)

elmon_x86_rhel52:  $(FILE)
	cc -o elmon_x86_rhel52 $(FILE) $(CFLAGS) $(LDFLAGS)

elmon_x86_rhel3:  $(FILE)
elmon_x86_rhel4:  $(FILE)
	cc -o elmon_x86_rhel4 $(FILE) $(CFLAGS) $(LDFLAGS)

elmon_x86_rhel3:  $(FILE)
	cc -o elmon_x86_rhel3 $(FILE) $(CFLAGS) $(LDFLAGS)

elmon_x86_redhat9:  $(FILE)
	cc -o elmon_x86_redhat9 $(FILE) $(CFLAGS) $(LDFLAGS)

elmon_x86_rhel2:
	cc -o elmon_x86_rhel2 $(FILE) $(CFLAGS) $(LDFLAGS) -D REREAD=1

elmon_x86_debian3:  $(FILE)
	cc -o elmon_x86_debian3 $(FILE) $(CFLAGS) $(LDFLAGS)

elmon_x86_fedora10: 
	cc -s -o elmon_x86_fedora10 $(FILE) $(CFLAGS) $(LDFLAGS)

elmon_x86_64_fedora10: 
	cc -s -o elmon_x86_64_fedora10 $(FILE) $(CFLAGS) $(LDFLAGS)

elmon_x86_ubuntu810: 
	cc -o elmon_x86_ubuntu810 $(FILE) $(CFLAGS) $(LDFLAGS)

elmon_x86_64_ubuntu810: 
	cc -o elmon_x86_64_ubuntu810 $(FILE) $(CFLAGS) $(LDFLAGS)

elmon_x86_ubuntu910: 
	cc -o elmon_x86_ubuntu910 $(FILE) $(CFLAGS) $(LDFLAGS)

elmon_x86_64_ubuntu910: 
	cc -o elmon_x86_64_ubuntu910 $(FILE) $(CFLAGS) $(LDFLAGS)

