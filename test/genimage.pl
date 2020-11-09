#!/usr/bin/perl
#
# WARNING WARNING WARNING: This is a *HUGE* hack!
#
# genimage.pl, version 2
#
# Author: Rajit Manohar
#

#
#  This is the objdump program to use
#
$objdump_pgm = "mips-sgi-irix5-objdump";

die "Usage: $0 <exe-file>" unless $#ARGV >= 0;
die "File $ARGV[0] doesn't exist!" unless -f $ARGV[0];

$tmpfile = $ARGV[0];
my $x = 0;

if ($#ARGV > 0) {
  shift;
  @list = @ARGV;
  $x = 1;
}
else {
  @list = (".text", 
	   ".data", 
	   ".rodata", 
	   ".reginfo", 
	   ".lit8",
	   ".lit4",
	   ".sdata",
	   ".got",
	   ".data0",
	   ".interp",
	   ".dynamic",
	   ".hash",
	   ".dynsym",
	   ".dynstr",
	   ".init",
	   ".mdebug",
	   ".compat_rel",
	   ".bss",
	   ".sbss"
	  );
  $x = 0;
}

my @seclen;
my @secstart;

&read_section_headers ($tmpfile, @list);

for ($i=0; $i <= $#list; $i++) {
  next if ($seclen[$i] == 0);
  next if ($list[$i] eq ".bss" || $list[$i] eq ".sbss");

  open(OBJ,"$objdump_pgm -j $list[$i] --full-contents $tmpfile|");

  $found = 0;
  while (<OBJ>) {
    if (/^Contents of section $list[$i]/) {
      $found = 1;
      last;
    }
  }

  next if $found == 0;

  if (($seclen[$i] & 7) == 0) {
    $found = 2;
    printf ("*0x00000000%08x %d\n", $secstart[$i], $seclen[$i]/8);
  }
  else {
    if (($seclen[$i] > 32) && ($seclen[$i] & 3) == 0) {
      $found = 3;
      printf ("*0x00000000%08x %d\n", $secstart[$i], ($seclen[$i]-4)/8);
    }
  }

  while (<OBJ>) {
    /\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)/ || /\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)/ || /\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)/;
    if ($found == 2 || $found == 3) {
      $seclen[$i] -= 8;
      if ($seclen[$i] >= 0) {
	printf ("0x%08x%08x\n", hex($2),hex($3));
      }
      elsif ($found == 3 && $seclen[$i] == -4) {
	printf ("0x00000000%08x 0x%08x%08x\n", hex($1), hex($2),hex($3));
      }
      $seclen[$i] -= 8;
      if ($seclen[$i] >= 0) {
	printf ("0x%08x%08x\n", hex($4),hex($5));
      }
      elsif ($found == 3 && $seclen[$i] == -4) {
	printf ("0x00000000%08x 0x%08x%08x\n", hex($1)+8, hex($4),hex($5));
      }
    }
    else {
      printf ("0x00000000%08x 0x%08x%08x\n", hex($1),hex($2),hex($3));
      printf ("0x00000000%08x 0x%08x%08x\n", hex($1)+8,hex($4),hex($5));
    }
  }
}

if ($x == 0) {
# dump .bss, .sbss areas... hmm...
  dump_zeros_for_section (".bss");
  dump_zeros_for_section (".sbss");
}

sub dump_zeros_for_section {
  my ($secname) = $_[0];
  my $i, $j;

  $i = &find_sec_index ($secname, @list);

  if ($i == -1 || $seclen[$i] == 0) {
    return;
  }

  if (($secstart[$i] & 7) != 0) {
    die "Need to fix this.\n";
  }
  printf ("\@0x00000000%08x 0x" . "0" x 16 . " " . ($seclen[$i]/8) . "\n",
	  $secstart[$i]);
  if (($seclen[$i] & 7) != 0) {
    printf ("+0x00000000%08x " . ($seclen[$i]&7) . "\n", $secstart[$i]+8*($seclen[$i]/8));
    for ($j=0; $j < ($seclen[$i]&7); $j++) {
      printf ("0x0\n");
    }
  }
  return;
}


sub read_section_headers {
  my ($file, @secs) = @_;

  my $i = 0;
  
  open(OBJ,"$objdump_pgm -h $file|");

  while (<OBJ>) {
    /^\s*(\S.*\S)\s*$/;
    $_ = $1;
    @line = split(/\s+/);
    $i = &find_sec_index ($line[1],@secs);
    next if $i == -1;
    $seclen[$i] = hex($line[2]);
    $secstart[$i] = hex($line[3]);
  }
  close (OBJ);
}

sub find_sec_index {
  my ($name, @secs) = @_;
  my $i;
  
  for ($i=0; $i <= $#secs; $i++) {
    if ($name eq $secs[$i]) {
      return $i;
    }
  }
  return -1;
}
