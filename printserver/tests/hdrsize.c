// Host-side verification of the cups_page_header2_t binary layout.
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

typedef unsigned cups_u32;
typedef float cups_float;

typedef struct {
  char MediaClass[64];
  char MediaColor[64];
  char MediaType[64];
  char OutputType[64];
  unsigned AdvanceDistance;
  unsigned AdvanceMedia;
  unsigned Collate;
  unsigned CutMedia;
  unsigned Duplex;
  unsigned HWResolution[2];
  unsigned ImagingBoundingBox[4];
  unsigned InsertSheet;
  unsigned Jog;
  unsigned LeadingEdge;
  unsigned Margins[2];
  unsigned ManualFeed;
  unsigned MediaPosition;
  unsigned MediaWeight;
  unsigned MirrorPrint;
  unsigned NegativePrint;
  unsigned NumCopies;
  unsigned Orientation;
  unsigned OutputFaceUp;
  unsigned PageSize[2];
  unsigned Separations;
  unsigned TraySwitch;
  unsigned Tumble;
  unsigned cupsWidth;
  unsigned cupsHeight;
  unsigned cupsMediaType;
  unsigned cupsBitsPerColor;
  unsigned cupsBitsPerPixel;
  unsigned cupsBytesPerLine;
  unsigned cupsColorOrder;
  unsigned cupsColorSpace;
  unsigned cupsCompression;
  unsigned cupsRowCount;
  unsigned cupsRowFeed;
  unsigned cupsRowStep;
  unsigned cupsNumColors;
  float cupsBorderlessScalingFactor;
  float cupsPageSize[2];
  float cupsImagingBBox[4];
  unsigned cupsInteger[16];
  float cupsReal[16];
  char cupsString[16][64];
  char cupsMarkerType[64];
  char cupsRenderingIntent[64];
  char cupsPageSizeName[64];
} cups_page_header2_t;

int main(void) {
  printf("sizeof(cups_page_header2_t) = %zu\n", sizeof(cups_page_header2_t));
  printf("offset MediaClass   = %zu\n", offsetof(cups_page_header2_t, MediaClass));
  printf("offset HWResolution = %zu\n", offsetof(cups_page_header2_t, HWResolution));
  printf("offset PageSize     = %zu\n", offsetof(cups_page_header2_t, PageSize));
  printf("offset cupsWidth    = %zu\n", offsetof(cups_page_header2_t, cupsWidth));
  printf("offset cupsHeight   = %zu\n", offsetof(cups_page_header2_t, cupsHeight));
  printf("offset cupsBitsPerPixel = %zu\n", offsetof(cups_page_header2_t, cupsBitsPerPixel));
  printf("offset cupsBytesPerLine = %zu\n", offsetof(cups_page_header2_t, cupsBytesPerLine));
  printf("offset cupsColorSpace   = %zu\n", offsetof(cups_page_header2_t, cupsColorSpace));
  printf("offset cupsInteger  = %zu\n", offsetof(cups_page_header2_t, cupsInteger));
  printf("offset cupsPageSizeName = %zu\n", offsetof(cups_page_header2_t, cupsPageSizeName));
  return 0;
}
