```
The tonegen peripheral can generate audio frequency square
waves with an adjustable volume.  You can specify the
frequency as a floating point number or as a music note.
Volume is in the range of 0 to 100 and follows an audio
taper.  Note duration is specified in milliseconds.


HARDWARE
  The tonegen peripheral assumes that the four FPGA pins
are tied to a 2R-R resistor network.  Note this is _NOT_
an R-2R network.  An R-2R network is linear while a 2R-R
network is very non-linear.  We use the non-linear nature
of the 2R-R network to help get a non-linear audio taper
to the volume.  The 2R-R network should be followed by a
low pass filter with a cutoff around 20KHz.  The cutoff
frequency of the low pass filter is not critical.


RESOURCES
You can specify a single note to play or you can specify
a melody file of notes to be played.

note:
   The note resource specifies the note frequency, volume,
and duration.  Frequency can be given as a floating point
value in the range of 10 to 10000 Hertz.  Frequency can
also be specified at one of Ax, A#x, Bx, Cx, C#x, Dx, D#x,
Ex, Fx, F#x, Gx, or G#x, where 'x' specifies the octave
in the range of 0 to 8.
   Volume is specified as a relative value in the range
of 0 to 100.  A value of zero turns off all audio output.
Volume follows a logarithmic curve to match the audio
taper of a typical sound system.
   Duration is specified in milliseconds in the range of
1 to 4095.

melody:
    The melody resource lets you play a file of notes.
Each line has a frequency, volume, and duration.  Lines
which do not parse as notes are treated as comments and
are quietly ignored.  Use a volume of zero to add a quiet
pause between notes.
    The daemon changes it working directory to / when it
starts.  For this reason all melody file names must be
fully qualified.  
    A typical file might appear as:
# Melody file with freq, vol, duration
g3 68 172
c4 73 172
e4 84 162
g4 83 384
e4 74 128
g4 86 1000


Examples:
    pcset tonegen note 1000 50 4000
    pcset tonegen note A4 100 100
    pcset tonegen melody /usr/local/share/my_melody.txt


```
