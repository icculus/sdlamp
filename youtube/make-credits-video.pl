#!/usr/bin/perl -w

# sudo apt-get install libimage-magick-perl

use warnings;
use strict;
use Image::Magick;
use File::Basename;
#use Data::Dumper;

# https://www.oreilly.com/library/view/perl-cookbook/1565922433/ch04s18.html
# fisher_yates_shuffle( \@array ) : generate a random permutation
# of @array in place
sub fisher_yates_shuffle {
    my $array = shift;
    my $i;
    for ($i = @$array; --$i; ) {
        my $j = int rand ($i+1);
        next if $i == $j;
        @$array[$i,$j] = @$array[$j,$i];
    }
}
sub usage {
    print STDERR "USAGE: $0 <inputvideo> <title>\n";
    exit(1);
}

my $patronstxtfname = dirname($0) . "/../patrons.txt";
my $inputvideofname = undef;
my $title = undef;
foreach (@ARGV) {
    $inputvideofname = $_, next if not defined $inputvideofname;
    $title = $_, next if not defined $title;
    usage();
}
usage() if not defined $inputvideofname;
usage() if not defined $title;

# Randomize the list so it's in a different order for each video!
open(FH, '<', $patronstxtfname) or die("Failed to open $patronstxtfname: $!\n");
my @creditstxt = <FH>;
close(FH);

my @credits = ();
foreach (@creditstxt) {
    chomp;
    s/\A\s+//;
    s/\s+\Z//;
    push @credits, $_ if ($_ ne '');
}
undef @creditstxt;

fisher_yates_shuffle( \@credits );

# if the credit list has one entry that would be on a row by itself, add an extra string.
if ((scalar(@credits) % 2) != 0) {
    push @credits, '(your name here!)';
}

my $giantfont = 'Ubuntu-Bold';
my $bigfont = 'Ubuntu-Bold';
my $smallfont = 'Liberation-Sans';
my $giantpointsize = 90;
my $bigpointsize = 60;
my $smallpointsize = 38;
my $num_credits = scalar(@credits);
my $imgw = 1920;
my $imgh = 1080;
my $titleimgfname = 'title.png';
my $titlevideofname = 'title.mp4';
my $creditsimgfname = 'credits.png';
my $creditsvideofname = 'credits.mp4';

unlink($titleimgfname);
unlink($titlevideofname);
unlink($creditsimgfname);
unlink($creditsvideofname);

#print Dumper($img->QueryFont());

# Do the credits card.
my $img = Image::Magick->new(size=>"${imgw}x${imgh}");
$img->ReadImage('xc:black');

my $x_ppem; my $y_ppem; my $ascender; my $descender; my $width; my $textheightgiant; my $textheightbig; my $textheightsmall; my $max_advance;
($x_ppem, $y_ppem, $ascender, $descender, $width, $textheightgiant, $max_advance) = $img->QueryFontMetrics(font => $giantfont, pointsize => $giantpointsize, text => 'W',);
($x_ppem, $y_ppem, $ascender, $descender, $width, $textheightbig, $max_advance) = $img->QueryFontMetrics(font => $bigfont, pointsize => $bigpointsize, text => 'W',);
($x_ppem, $y_ppem, $ascender, $descender, $width, $textheightsmall, $max_advance) = $img->QueryFontMetrics(font => $smallfont, pointsize => $smallpointsize, text => 'W');

my $y = $textheightbig;
$img->Annotate(font => $bigfont, pointsize => $bigpointsize, x => 0, y => $y, gravity => 'North', fill => 'white', text => "Thanks to my patrons!", antialias => 'true');
#$y += $textheightbig * 1.5;

#my $creditsheight = ($imgh - ($y * 2));
my $creditsheight = ($num_credits / 2) * ($textheightsmall * 1.1);
#my $credits_y_start = $y;
my $credits_x_start = $imgw / 8;
my $credits_y_start = (($imgh - $creditsheight) / 2) + $textheightsmall;
my $credits_x_advance = $imgw / 2;
my $credits_y_advance = $textheightsmall * 1.1;

my $x = $credits_x_start;
$y = $credits_y_start;

my $idx = 0;
foreach (@credits) {
    $img->Annotate(font => $smallfont, pointsize => $smallpointsize, x => $x, y => $y, gravity => 'NorthWest', fill => 'white', antialias => 'true', text => $_);
    if ($idx & 1) {
        $x = $credits_x_start;
        $y += $credits_y_advance;
    } else {
        $x += $credits_x_advance;
    }
    $idx++;
}

$img->Annotate(font => $bigfont, pointsize => $bigpointsize, x => 0, y => $imgh - ($textheightbig * 1.5), gravity => 'North', fill => 'white', text => "Want your name here?     https://patreon.com/icculus", antialias => 'true');

open(FH, '>', $creditsimgfname) or die("Failed to open '$creditsimgfname' for write: $!\n");
$img->Write(file => \*FH, filename => $creditsimgfname);
close(FH);
system("ffmpeg -f lavfi -i anullsrc=channel_layout=stereo:sample_rate=48000 -loop 1 -i $creditsimgfname -r 30 -pix_fmt yuv420p -t 10 $creditsvideofname");

# Do the title card.
$img = Image::Magick->new(size=>"${imgw}x${imgh}");
$img->ReadImage('xc:black');
$img->Annotate(font => $giantfont, pointsize => $giantpointsize, x => 0, y => 0, gravity => 'Center', fill => 'white', text => $title, antialias => 'true');
open(FH, '>', $titleimgfname) or die("Failed to open '$titleimgfname' for write: $!\n");
$img->Write(file => \*FH, filename => $titleimgfname);
close(FH);
system("ffmpeg -f lavfi -i anullsrc=channel_layout=stereo:sample_rate=48000 -loop 1 -i $titleimgfname -r 30 -pix_fmt yuv420p -t 5 $titlevideofname");

# Build the final video!
system("./ffmpeg_concat_xfade.py $titlevideofname $inputvideofname $creditsvideofname -o `date +%Y-%m-%d_%H-%M-%S`.mp4");

# end of make-credits-video.pl ...

