/**********************************************************************

  Audacity: A Digital Audio Editor

  Envelope.cpp

  Dominic Mazzoni (original author)
  Dr William Bland (integration - the Calculus kind)
  Monty (xiphmont) (important bug fixes)

*******************************************************************//**

\class Envelope
\brief Draggable curve used in TrackPanel for varying amplification.

  This class manages an envelope - i.e. a piecewise linear funtion
  that the user can edit by dragging control points around.  The
  envelope is most commonly used to control the amplitude of a
  waveform, but it is also used to shape the Equalization curve.

*//****************************************************************//**

\class EnvPoint
\brief EnvPoint, derived from XMLTagHandler, provides Envelope with
a draggable point type.

*//*******************************************************************/

#include "Envelope.h"
#include "ViewInfo.h"

#include <math.h>

#include <wx/dc.h>
#include <wx/brush.h>
#include <wx/event.h>
#include <wx/pen.h>
#include <wx/textfile.h>
#include <wx/log.h>

#include "AColor.h"
#include "DirManager.h"
#include "TrackArtist.h"

Envelope::Envelope(bool exponential, double minValue, double maxValue, double defaultValue)
   : mDB(exponential)
   , mMinValue(minValue)
   , mMaxValue(maxValue)
   , mDefaultValue { ClampValue(defaultValue) }
{
}

Envelope::~Envelope()
{
}

/// Rescale function for time tracks (could also be used for other tracks though).
/// This is used to load old time track project files where the envelope used a 0 to 1
/// range instead of storing the actual time track values. This function will change the range of the envelope
/// and rescale all envelope points accordingly (unlike SetRange, which clamps the envelope points to the NEW range).
/// @minValue - the NEW minimum value
/// @maxValue - the NEW maximum value
void Envelope::RescaleValues(double minValue, double maxValue)
{
   double oldMinValue = mMinValue;
   double oldMaxValue = mMaxValue;
   mMinValue = minValue;
   mMaxValue = maxValue;

   // rescale the default value
   double factor = (mDefaultValue - oldMinValue) / (oldMaxValue - oldMinValue);
   mDefaultValue = ClampValue(mMinValue + (mMaxValue - mMinValue) * factor);

   // rescale all points
   for( unsigned int i = 0; i < mEnv.size(); i++ ) {
      factor = (mEnv[i].GetVal() - oldMinValue) / (oldMaxValue - oldMinValue);
      mEnv[i].SetVal( this, mMinValue + (mMaxValue - mMinValue) * factor );
   }

}

/// Flatten removes all points from the envelope to
/// make it horizontal at a chosen y-value.
/// @value - the y-value for the flat envelope.
void Envelope::Flatten(double value)
{
   mEnv.clear();
   mDefaultValue = ClampValue(value);
}

void Envelope::SetDragPoint(int dragPoint)
{
   mDragPoint = std::max(-1, std::min(int(mEnv.size() - 1), dragPoint));
   mDragPointValid = (mDragPoint >= 0);
}

void Envelope::SetDragPointValid(bool valid)
{
   mDragPointValid = (valid && mDragPoint >= 0);
   if (mDragPoint >= 0 && !valid) {
      // We're going to be deleting the point; On
      // screen we show this by having the envelope move to
      // the position it will have after deletion of the point.
      // Without deleting the point we move it left or right
      // to the same position as the previous or next point.

      static const double big = std::numeric_limits<double>::max();
      auto size = mEnv.size();

      if( size <= 1) {
         // There is only one point - just move it
         // off screen and at default height.
         // temporary state when dragging only!
         mEnv[mDragPoint].SetT(big);
         mEnv[mDragPoint].SetVal( this, mDefaultValue );
         return;
      }
      else if ( mDragPoint + 1 == size ) {
         // Put the point at the height of the last point, but also off screen.
         mEnv[mDragPoint].SetT(big);
         mEnv[mDragPoint].SetVal( this, mEnv[ size - 1 ].GetVal() );
      }
      else {
         // Place it exactly on its right neighbour.
         // That way the drawing code will overpaint the dark dot with
         // a light dot, as if it were deleted.
         const auto &neighbor = mEnv[mDragPoint + 1];
         mEnv[mDragPoint].SetT(neighbor.GetT());
         mEnv[mDragPoint].SetVal( this, neighbor.GetVal() );
      }
   }
}

void Envelope::MoveDragPoint(double newWhen, double value)
{
   SetDragPointValid(true);
   if (!mDragPointValid)
      return;

   // We'll limit the drag point time to be between those of the preceding
   // and next envelope point.
   double limitLo = 0.0;
   double limitHi = mTrackLen;

   if (mDragPoint > 0)
      limitLo = std::max(limitLo, mEnv[mDragPoint - 1].GetT());
   if (mDragPoint + 1 < mEnv.size())
      limitHi = std::min(limitHi, mEnv[mDragPoint + 1].GetT());

   EnvPoint &dragPoint = mEnv[mDragPoint];
   const double tt =
      std::max(limitLo, std::min(limitHi, newWhen));

   // This might temporary violate the constraint that at most two
   // points share a time value.
   dragPoint.SetT(tt);
   dragPoint.SetVal( this, value );
}

void Envelope::ClearDragPoint()
{
   if (!mDragPointValid && mDragPoint >= 0)
      Delete(mDragPoint);

   mDragPoint = -1;
   mDragPointValid = false;
}

void Envelope::SetRange(double minValue, double maxValue) {
   mMinValue = minValue;
   mMaxValue = maxValue;
   mDefaultValue = ClampValue(mDefaultValue);
   for( unsigned int i = 0; i < mEnv.size(); i++ )
      mEnv[i].SetVal( this, mEnv[i].GetVal() ); // this clamps the value to the NEW range
}

// This is used only during construction of an Envelope by complete or partial
// copy of another, or when truncating a track.
void Envelope::AddPointAtEnd( double t, double val )
{
   mEnv.push_back( EnvPoint{ t, val } );

   // Assume copied points were stored by nondecreasing time.
   // Allow no more than two points at exactly the same time.
   // Maybe that happened, because extra points were inserted at the boundary
   // of the copied range, which were not in the source envelope.
   auto nn = mEnv.size() - 1;
   while ( nn >= 2 && mEnv[ nn - 2 ].GetT() == t ) {
      // Of three or more points at the same time, erase one in the middle,
      // not the one newly added.
      mEnv.erase( mEnv.begin() + nn - 1 );
      --nn;
   }
}

Envelope::Envelope(const Envelope &orig, double t0, double t1)
   : mDB(orig.mDB)
   , mMinValue(orig.mMinValue)
   , mMaxValue(orig.mMaxValue)
   , mDefaultValue(orig.mDefaultValue)
{
   mOffset = wxMax(t0, orig.mOffset);
   mTrackLen = wxMin(t1, orig.mOffset + orig.mTrackLen) - mOffset;

   auto range1 = orig.EqualRange( t0 - orig.mOffset, 0 );
   auto range2 = orig.EqualRange( t1 - orig.mOffset, 0 );
   CopyRange(orig, range1.first, range2.second);
}

Envelope::Envelope(const Envelope &orig)
   : mDB(orig.mDB)
   , mMinValue(orig.mMinValue)
   , mMaxValue(orig.mMaxValue)
   , mDefaultValue(orig.mDefaultValue)
{
   mOffset = orig.mOffset;
   mTrackLen = orig.mTrackLen;
   CopyRange(orig, 0, orig.GetNumberOfPoints());
}

void Envelope::CopyRange(const Envelope &orig, size_t begin, size_t end)
{
   int len = orig.mEnv.size();
   int i = begin;

   // Create the point at 0 if it needs interpolated representation
   if ( i > 0 )
      AddPointAtEnd(0, orig.GetValue(mOffset));

   // Copy points from inside the copied region
   for (; i < end; ++i) {
      const EnvPoint &point = orig[i];
      const double when = point.GetT() + (orig.mOffset - mOffset);
      AddPointAtEnd(when, point.GetVal());
   }

   // Create the final point if it needs interpolated representation
   // If the last point of e was exatly at t1, this effectively copies it too.
   if (mTrackLen > 0 && i < len)
      AddPointAtEnd( mTrackLen, orig.GetValue(mOffset + mTrackLen));
}

/// Limit() limits a double value to a range.
/// TODO: Move to a general utilities source file.
static double Limit( double Lo, double Value, double Hi )
{
   if( Value < Lo )
      return Lo;
   if( Value > Hi )
      return Hi;
   return Value;
}

/// TODO: This should probably move to track artist.
static void DrawPoint(wxDC & dc, const wxRect & r, int x, int y, bool top)
{
   if (y >= 0 && y <= r.height) {
      wxRect circle(r.x + x, r.y + (top ? y - 1: y - 2), 4, 4);
      dc.DrawEllipse(circle);
   }
}

/// TODO: This should probably move to track artist.
void Envelope::DrawPoints(wxDC & dc, const wxRect & r, const ZoomInfo &zoomInfo,
                    bool dB, double dBRange,
                    float zoomMin, float zoomMax, bool mirrored) const
{
   dc.SetPen(AColor::envelopePen);
   dc.SetBrush(*wxWHITE_BRUSH);

   for (int i = 0; i < (int)mEnv.size(); i++) {
      const double time = mEnv[i].GetT() + mOffset;
      const wxInt64 position = zoomInfo.TimeToPosition(time);
      if (position >= 0 && position < r.width) {
         // Change colour if this is the draggable point...
         if (i == mDragPoint) {
            dc.SetPen(AColor::envelopePen);
            dc.SetBrush(AColor::envelopeBrush);
         }

         double v = mEnv[i].GetVal();
         int x = (int)(position);
         int y, y2;

         y = GetWaveYPos(v, zoomMin, zoomMax, r.height, dB,
            true, dBRange, false);
         if (!mirrored) {
            DrawPoint(dc, r, x, y, true);
         }
         else {
            y2 = GetWaveYPos(-v-.000000001, zoomMin, zoomMax, r.height, dB,
               true, dBRange, false);

            // This follows the same logic as the envelop drawing in
            // TrackArtist::DrawEnvelope().
            // TODO: make this calculation into a reusable function.
            if (y2 - y < 9) {
               int value = (int)((zoomMax / (zoomMax - zoomMin)) * r.height);
               y = value - 4;
               y2 = value + 4;
            }

            DrawPoint(dc, r, x, y, true);
            DrawPoint(dc, r, x, y2, false);

            // Contour
            y = GetWaveYPos(v, zoomMin, zoomMax, r.height, dB,
               false, dBRange, false);
            y2 = GetWaveYPos(-v-.000000001, zoomMin, zoomMax, r.height, dB,
               false, dBRange, false);
            if (y <= y2) {
               DrawPoint(dc, r, x, y, true);
               DrawPoint(dc, r, x, y2, false);
            }
         }

         // Change colour back again if was the draggable point.
         if (i == mDragPoint) {
            dc.SetPen(AColor::envelopePen);
            dc.SetBrush(*wxWHITE_BRUSH);
         }
      }
   }
}

bool Envelope::HandleXMLTag(const wxChar *tag, const wxChar **attrs)
{
   // Return unless it's the envelope tag.
   if (wxStrcmp(tag, wxT("envelope")))
      return false;

   int numPoints = 0;
   long nValue = -1;

   while (*attrs) {
      const wxChar *attr = *attrs++;
      const wxChar *value = *attrs++;
      if (!value)
         break;
      const wxString strValue = value;
      if( !wxStrcmp(attr, wxT("numpoints")) &&
            XMLValueChecker::IsGoodInt(strValue) && strValue.ToLong(&nValue))
         numPoints = nValue;
   }
   if (numPoints < 0)
      return false;

   mEnv.clear();
   mEnv.reserve(numPoints);
   return true;
}

XMLTagHandler *Envelope::HandleXMLChild(const wxChar *tag)
{
   if (wxStrcmp(tag, wxT("controlpoint")))
      return NULL;

   mEnv.push_back( EnvPoint{} );
   return &mEnv.back();
}

void Envelope::WriteXML(XMLWriter &xmlFile) const
// may throw
{
   unsigned int ctrlPt;

   xmlFile.StartTag(wxT("envelope"));
   xmlFile.WriteAttr(wxT("numpoints"), mEnv.size());

   for (ctrlPt = 0; ctrlPt < mEnv.size(); ctrlPt++) {
      const EnvPoint &point = mEnv[ctrlPt];
      xmlFile.StartTag(wxT("controlpoint"));
      xmlFile.WriteAttr(wxT("t"), point.GetT(), 12);
      xmlFile.WriteAttr(wxT("val"), point.GetVal(), 12);
      xmlFile.EndTag(wxT("controlpoint"));
   }

   xmlFile.EndTag(wxT("envelope"));
}

namespace
{
inline int SQR(int x) { return x * x; }
}

/// ValueOfPixel() converts a y position on screen to an envelope value.
/// @param y - y position, usually of the mouse.relative to the clip.
/// @param height - height of the rectangle we are in.
/// @upper - true if we are on the upper line, false if on lower.
/// @dB - display mode either linear or log.
/// @zoomMin - vertical scale, typically -1.0
/// @zoomMax - vertical scale, typically +1.0
float EnvelopeEditor::ValueOfPixel( int y, int height, bool upper,
                              bool dB, double dBRange,
                              float zoomMin, float zoomMax)
{
   float v = ::ValueOfPixel(y, height, 0 != mContourOffset, dB, dBRange, zoomMin, zoomMax);

   // MB: this is mostly equivalent to what the old code did, I'm not sure
   // if anything special is needed for asymmetric ranges
   if(upper)
      return mEnvelope.ClampValue(v);
   else
      return mEnvelope.ClampValue(-v);
}

/// HandleMouseButtonDown either finds an existing control point or adds a NEW one
/// which is then recorded as the point to drag.
/// This is slightly complicated by there possibly being four control points for
/// a given time value:
/// We have an upper and lower envelope line.
/// Also we may be showing an inner envelope (at 0.5 the range).
bool EnvelopeEditor::HandleMouseButtonDown(const wxMouseEvent & event, wxRect & r,
                                     const ZoomInfo &zoomInfo,
                                     bool dB, double dBRange,
                                     float zoomMin, float zoomMax)
{
   int ctr = (int)(r.height * zoomMax / (zoomMax - zoomMin));
   bool upper = !mMirrored || (zoomMin >= 0.0) || (event.m_y - r.y < ctr);

   int clip_y = event.m_y - r.y;
   if(clip_y < 0) clip_y = 0; //keeps point in rect r, even if mouse isn't
   if(clip_y > r.GetBottom()) clip_y = r.GetBottom();

   int bestNum = -1;
   int bestDistSqr = 100; // Must be within 10 pixel radius.

   // Member variables hold state that will be needed in dragging.
   mButton        = event.GetButton();
   mContourOffset = false;

   //   wxLogDebug(wxT("Y:%i Height:%i Offset:%i"), y, height, mContourOffset );
   int len = mEnvelope.GetNumberOfPoints();

   // TODO: extract this into a function FindNearestControlPoint()
   // TODO: also fix it so that we can drag the last point on an envelope.
   for (int i = 0; i < len; i++) { //search for control point nearest click
      const double time = mEnvelope[i].GetT() + mEnvelope.GetOffset();
      const wxInt64 position = zoomInfo.TimeToPosition(time);
      if (position >= 0 && position < r.width) {

         int x = (int)(position);
         int y[4];
         int numControlPoints;

         // Outer control points
         double value = mEnvelope[i].GetVal();
         y[0] = GetWaveYPos(value, zoomMin, zoomMax, r.height,
                                dB, true, dBRange, false);
         y[1] = GetWaveYPos(-value, zoomMin, zoomMax, r.height,
                                dB, true, dBRange, false);

         // Inner control points(contour)
         y[2] = GetWaveYPos(value, zoomMin, zoomMax, r.height,
                                dB, false, dBRange, false);
         y[3] = GetWaveYPos(-value -.00000001, zoomMin, zoomMax,
                                r.height, dB, false, dBRange, false);

         numControlPoints = 4;

         if (y[2] > y[3])
            numControlPoints = 2;

         if (!mMirrored)
            numControlPoints = 1;

         const int deltaXSquared = SQR(x - (event.m_x - r.x));
         for(int j=0; j<numControlPoints; j++){

            const int dSqr = deltaXSquared + SQR(y[j] - (event.m_y - r.y));
            if (dSqr < bestDistSqr) {
               bestNum = i;
               bestDistSqr = dSqr;
               mContourOffset = (bool)(j > 1);
            }
         }
      }
   }

   if (bestNum >= 0) {
      mEnvelope.SetDragPoint(bestNum);
   }
   else {
      // TODO: Extract this into a function CreateNewPoint
      const double when = zoomInfo.PositionToTime(event.m_x, r.x);

      //      if (when <= 0 || when >= mTrackLen)
      //         return false;

      const double v = mEnvelope.GetValue( when );

      int ct = GetWaveYPos( v, zoomMin, zoomMax, r.height, dB,
                               false, dBRange, false) ;
      int cb = GetWaveYPos( -v-.000000001, zoomMin, zoomMax, r.height, dB,
                               false, dBRange, false) ;
      if (ct <= cb || !mMirrored) {
         int t = GetWaveYPos( v, zoomMin, zoomMax, r.height, dB,
                                 true, dBRange, false) ;
         int b = GetWaveYPos( -v, zoomMin, zoomMax, r.height, dB,
                                 true, dBRange, false) ;

         ct = (t + ct) / 2;
         cb = (b + cb) / 2;

         if (mMirrored &&
            (event.m_y - r.y) > ct &&
            ((event.m_y - r.y) < cb))
            mContourOffset = true;
         else
            mContourOffset = false;
      }

      double newVal = ValueOfPixel(clip_y, r.height, upper, dB, dBRange,
                                   zoomMin, zoomMax);

      mEnvelope.SetDragPoint(mEnvelope.InsertOrReplaceRelative(when, newVal));
      mDirty = true;
   }

   mUpper = upper;

   // const int dragPoint = mEnvelope.GetDragPoint();
   // mInitialVal = mEnvelope[dragPoint].GetVal();
   // mInitialY = event.m_y+mContourOffset;

   return true;
}

void EnvelopeEditor::MoveDragPoint(const wxMouseEvent & event, wxRect & r,
                               const ZoomInfo &zoomInfo, bool dB, double dBRange,
                               float zoomMin, float zoomMax)
{
   int clip_y = event.m_y - r.y;
   if(clip_y < 0) clip_y = 0;
   if(clip_y > r.height) clip_y = r.height;
   double newVal = ValueOfPixel(clip_y, r.height, mUpper, dB, dBRange,
                                zoomMin, zoomMax);

   // We no longer tolerate multiple envelope points at the same t.
   // epsilon is less than the time offset of a single sample
   // TODO: However because mTrackEpsilon assumes 200KHz this use
   // of epsilon is a tad bogus.  What we need to do instead is DELETE
   // a duplicated point on a mouse up.
   double newWhen = zoomInfo.PositionToTime(event.m_x, r.x) - mEnvelope.GetOffset();
   mEnvelope.MoveDragPoint(newWhen, newVal);
}

bool EnvelopeEditor::HandleDragging(const wxMouseEvent & event, wxRect & r,
                               const ZoomInfo &zoomInfo, bool dB, double dBRange,
                               float zoomMin, float zoomMax,
                               float WXUNUSED(eMin), float WXUNUSED(eMax))
{
   mDirty = true;

   wxRect larger = r;
   larger.Inflate(10, 10);

   if (larger.Contains(event.m_x, event.m_y))
   {
      // IF we're in the rect THEN we're not deleting this point (anymore).
      // ...we're dragging it.
      MoveDragPoint( event, r, zoomInfo, dB, dBRange, zoomMin, zoomMax);
      return true;
   }

   if(!mEnvelope.GetDragPointValid())
      // IF we already know we're deleting THEN no envelope point to update.
      return false;

   // Invalidate the point
   mEnvelope.SetDragPointValid(false);
   return true;
}

// Exit dragging mode and delete dragged point if neccessary.
bool EnvelopeEditor::HandleMouseButtonUp()
{
   mEnvelope.ClearDragPoint();
   mButton = wxMOUSE_BTN_NONE;
   return true;
}

void Envelope::Delete( int point )
{
   mEnv.erase(mEnv.begin() + point);
}

void Envelope::Insert(int point, const EnvPoint &p)
{
   mEnv.insert(mEnv.begin() + point, p);
}

// Returns true if parent needs to be redrawn
bool EnvelopeEditor::MouseEvent(const wxMouseEvent & event, wxRect & r,
                          const ZoomInfo &zoomInfo, bool dB, double dBRange,
                          float zoomMin, float zoomMax)
{
   if (event.ButtonDown() && mButton == wxMOUSE_BTN_NONE)
      return HandleMouseButtonDown( event, r, zoomInfo, dB, dBRange,
                                    zoomMin, zoomMax);
   if (event.Dragging() && mEnvelope.GetDragPoint() >= 0)
      return HandleDragging( event, r, zoomInfo, dB, dBRange,
                             zoomMin, zoomMax);
   if (event.ButtonUp() && event.GetButton() == mButton)
      return HandleMouseButtonUp();
   return false;
}

void Envelope::CollapseRegion( double t0, double t1, double sampleTime )
// NOFAIL-GUARANTEE
{
   // This gets called when somebody clears samples.

   // Snip points in the interval (t0, t1), shift values left at times after t1.
   // For the boundaries of the interval, preserve the left-side limit at the
   // start and right-side limit at the end.

   const auto epsilon = sampleTime / 2;
   t0 = std::max( 0.0, std::min( mTrackLen, t0 - mOffset ) );
   t1 = std::max( 0.0, std::min( mTrackLen, t1 - mOffset ) );

   // Determine the start of the range of points to remove from the array.
   auto range0 = EqualRange( t0, 0 );
   auto begin = range0.first;
   if ( begin == range0.second ) {
      if ( t0 > epsilon ) {
         // There was no point exactly at t0;
         // insert a point to preserve the value.
         auto val = GetValueRelative( t0 );
         InsertOrReplaceRelative( t0, val );
         ++begin;
      }
   }
   else
      // We will keep the first (or only) point that was at t0.
      ++begin;

   // We want end to be the index one past the range of points to remove from
   // the array.
   // At first, find index of the first point after t1:
   auto range1 = EqualRange( t1, 0 );
   auto end = range1.second;
   if ( range1.first == end ) {
      if ( mTrackLen - t1 > epsilon ) {
         // There was no point exactly at t1; insert a point to preserve the value.
         auto val = GetValueRelative( t1 );
         InsertOrReplaceRelative( t1, val );
         // end is now the index of this NEW point and that is correct.
      }
   }
   else
      // We will keep the last (or only) point that was at t1.
      --end;

   mEnv.erase( mEnv.begin() + begin, mEnv.begin() + end );

   // Shift points left after deleted region.
   auto len = mEnv.size();
   for ( size_t i = begin; i < len; ++i ) {
      auto &point = mEnv[i];
      point.SetT( point.GetT() - (t1 - t0) );
   }

   mTrackLen -= ( t1 - t0 );
}

// This operation is trickier than it looks; the basic rub is that
// a track's envelope runs the range from t=0 to t=tracklen; the t=0
// envelope point applies to the first sample, but the t=tracklen
// envelope point applies one-past the last actual sample.
// Rather than going to a .5-offset-index, we special case the framing.
void Envelope::Paste(double t0, const Envelope *e)
// NOFAIL-GUARANTEE
{
   const bool wasEmpty = (this->mEnv.size() == 0);

   // JC: The old analysis of cases and the resulting code here is way more complex than needed.
   // TODO: simplify the analysis and simplify the code.

   if (e->mEnv.size() == 0 && wasEmpty && e->mDefaultValue == this->mDefaultValue)
   {
      // msmeyer: The envelope is empty and has the same default value, so
      // there is nothing that must be inserted, just return. This avoids
      // the creation of unnecessary duplicate control points
      // MJS: but the envelope does get longer
      mTrackLen += e->mTrackLen;
      return;
   }

   t0 = wxMin(t0 - mOffset, mTrackLen);   // t0 now has origin of zero
   double deltat = e->mTrackLen;

   unsigned int i;
   unsigned int pos = 0;
   bool someToShift = false;
   bool atStart = false;
   bool beforeStart = false;
   bool atEnd = false;
   bool afterEnd = false;
   bool onPoint = false;
   unsigned int len = mEnv.size();

   // get values to perform framing of the insertion
   double splitval = GetValue(t0 + mOffset);

/*
Old analysis of cases:
(see discussions on audacity-devel around 19/8/7 - 23/8/7 and beyond, "Envelopes and 'Join'")
1  9     11  2    3 5  7   8   6 4   13              12
0-----0--0---0    -----0---0------       --(0)----

1   The insert point is at the beginning of the current env, and it is a control point.
2   The insert point is at the end of the current env, and it is a control point.
3   The insert point is at the beginning of the current env, and it is not a control point.
4   The insert point is at the end of the current env, and it is not a control point.
5   The insert point is not at a control point, and there is space either side.
6   As 5.
7   The insert point is at a control point, and there is space either side.
8   Same as 7.
9   Same as 5.
10  There are no points in the current envelope (commonly called by the 'undo' stuff, and not in the diagrams).
11  As 7.
12  Insert beyond the RH end of the current envelope (should not happen, at the moment)
13  Insert beyond the LH end of the current envelope (should not happen, at the moment)
*/

// JC: Simplified Analysis:
// In pasting in a clip we choose to preserve the envelope so that the loudness of the
// parts is unchanged.
//
// 1) This may introduce a discontinuity in the envelope at a boundary between the
//    old and NEW clips.  In that case we must ensure there are envelope points
//    at sample positions immediately before and immediately after the boundary.
// 2) If the points have the same value we only need one of them.
// 3) If the points have the same value AND it is the same as the value interpolated
//    from the rest of the envelope then we don't need it at all.
//
// We do the same for the left and right edge of the NEW clip.
//
// Even simpler: we could always add two points at a boundary and then call
// RemoveUnneededPoints() (provided that function behaves correctly).

   // See if existing points need shifting to the right, and what Case we are in
   if(len != 0) {
      // Not case 10: there are point/s in the envelope
      for (i = 0; i < len; i++) {
         if (mEnv[i].GetT() > t0)
            someToShift = true;
         else {
            pos = i; // last point not moved
            if ( fabs(mEnv[i].GetT() - t0) - 1/500000.0 < 0.0 ) // close enough to a point
               onPoint = true;
         }
      }

      // In these statements, remember we subtracted mOffset from t0
      if( t0 < mTrackEpsilon )
         atStart = true;
      if( (mTrackLen - t0) < mTrackEpsilon )
         atEnd = true;
      if(0 > t0)
         // Case 13
         beforeStart = true;
      if(mTrackLen < t0)
         // Case 12
         afterEnd = true;

      // Now test for the various Cases, and try to do the right thing
      if(atStart) {
         // insertion at the beginning
         if(onPoint) {
            // Case 1: move it R slightly to avoid duplicate point
            // first env point is at LH end
            mEnv[0].SetT(mEnv[0].GetT() + mTrackEpsilon);
            someToShift = true;  // there is now, even if there wasn't before
            //wxLogDebug(wxT("Case 1"));
         }
         else {
            // Case 3: insert a point to maintain the envelope
            InsertOrReplaceRelative(t0 + mTrackEpsilon, splitval);
            someToShift = true;
            //wxLogDebug(wxT("Case 3"));
         }
      }
      else {
         if(atEnd) {
            // insertion at the end
            if(onPoint) {
               // last env point is at RH end, Case 2:
               // move it L slightly to avoid duplicate point
               mEnv[0].SetT(mEnv[0].GetT() - mTrackEpsilon);
               //wxLogDebug(wxT("Case 2"));
            }
            else {
               // Case 4:
               // insert a point to maintain the envelope
               InsertOrReplaceRelative(t0 - mTrackEpsilon, splitval);
               //wxLogDebug(wxT("Case 4"));
            }
         }
         else if(onPoint) {
            // Case 7: move the point L and insert a NEW one to the R
            mEnv[pos].SetT(mEnv[pos].GetT() - mTrackEpsilon);
            InsertOrReplaceRelative(t0 + mTrackEpsilon, splitval);
            someToShift = true;
            //wxLogDebug(wxT("Case 7"));
         }
         else if( !beforeStart && !afterEnd ) {
            // Case 5: Insert points to L and R
            InsertOrReplaceRelative(t0 - mTrackEpsilon, splitval);
            InsertOrReplaceRelative(t0 + mTrackEpsilon, splitval);
            someToShift = true;
            //wxLogDebug(wxT("Case 5"));
         }
         else if( beforeStart ) {
            // Case 13:
            //wxLogDebug(wxT("Case 13"));
         }
         else {
            // Case 12:
            //wxLogDebug(wxT("Case 12"));
         }
      }

      // Now shift existing points to the right, if required
      if(someToShift) {
         len = mEnv.size();  // it may well have changed
         for (i = 0; i < len; i++)
            if (mEnv[i].GetT() > t0)
               mEnv[i].SetT(mEnv[i].GetT() + deltat);
      }
      mTrackLen += deltat;
   }
   else {
      // Case 10:
      if( mTrackLen == 0 ) // creating a NEW envelope
      {
         mTrackLen = e->mTrackLen;
         mOffset = e->mOffset;
         //wxLogDebug(wxT("Case 10, NEW env/clip: mTrackLen %f mOffset %f t0 %f"), mTrackLen, mOffset, t0);
      }
      else
      {
         mTrackLen += e->mTrackLen;
         //wxLogDebug(wxT("Case 10, paste into current env: mTrackLen %f mOffset %f t0 %f"), mTrackLen, mOffset, t0);
      }
   }

   // Copy points from inside the selection

   if (!wasEmpty) {
      // Add end points in case they are not not in e.
      // If they are in e, no harm, because the repeated Insert
      // calls for the start and end times will have no effect.
      const double leftval = e->GetValue(0 + e->mOffset);
      const double rightval = e->GetValue(e->mTrackLen + e->mOffset);
      InsertOrReplaceRelative(t0, leftval);
      InsertOrReplaceRelative(t0 + e->mTrackLen, rightval);
   }

   len = e->mEnv.size();
   for (i = 0; i < len; i++)
      InsertOrReplaceRelative(t0 + e->mEnv[i].GetT(), e->mEnv[i].GetVal());

/*   if(len != 0)
      for (i = 0; i < mEnv.size(); i++)
         wxLogDebug(wxT("Fixed i %d when %.18f val %f"),i,mEnv[i].GetT(),mEnv[i].GetVal()); */
}

// Deletes 'unneeded' points, starting from the left.
// If 'time' is set and positive, just deletes points in a small region
// around that value.
// 'Unneeded' means that the envelope doesn't change by more than
// 'tolerence' without the point being there.
void Envelope::RemoveUnneededPoints(double time, double tolerence)
// NOFAIL-GUARANTEE
{
   unsigned int len = mEnv.size();
   unsigned int i;
   double when, val, val1;

   if(mEnv.size() == 0)
      return;

   for (i = 0; i < len; i++) {
      when = mEnv[i].GetT();
      if(time >= 0)
      {
         if(fabs(when + mOffset - time) > 0.00025) // 2 samples at 8kHz, 11 at 44.1kHz
            continue;
      }
      val = mEnv[i].GetVal();
      Delete(i);  // try it to see if it's doing anything
      val1 = GetValue(when + mOffset);
      bool bExcludePoint = true;
      if( fabs(val -val1) > tolerence )
      {
         InsertOrReplaceRelative(when, val); // put it back, we needed it

         //Insert may have modified instead of inserting, if two points were at the same time.
         // in which case len needs to shrink i and len, because the array size decreased.
         bExcludePoint = (mEnv.size() < len);
      }

      if( bExcludePoint ) {   // it made no difference so leave it out
         len--;
         i--;
      }
   }
}

void Envelope::InsertSpace( double t0, double tlen )
// NOFAIL-GUARANTEE
{
   t0 -= mOffset;

   // Preserve the left-side limit at the split.
   auto val = GetValueRelative( t0 );
   auto range = EqualRange( t0, 0 );

   size_t index;
   if ( range.first < range.second )
      // There is already a control point.
      index = 1 + range.first;
   else
      // Make a control point.
      index = 1 + InsertOrReplaceRelative( t0, val );

   // Shift points.
   auto len = mEnv.size();
   for ( ; index < len; ++index ) {
      auto &point = mEnv[ index ];
      point.SetT( point.GetT() + tlen );
   }

   // increase track len, before insert or replace,
   // since it range chacks the values.
   mTrackLen += tlen;
   // Preserve the right-side limit.
   if ( 1 + range.first < range.second )
      // There was a control point already.
      ;
   else
      InsertOrReplaceRelative( t0 + tlen, val );
}

int Envelope::Reassign(double when, double value)
{
   when -= mOffset;

   int len = mEnv.size();
   if (len == 0)
      return -1;

   int i = 0;
   while (i < len && when > mEnv[i].GetT())
      i++;

   if (i >= len || when < mEnv[i].GetT())
      return -1;

   mEnv[i].SetVal( this, value );
   return 0;
}


size_t Envelope::GetNumberOfPoints() const
{
   return mEnv.size();
}

void Envelope::GetPoints(double *bufferWhen,
                         double *bufferValue,
                         int bufferLen) const
{
   int n = mEnv.size();
   if (n > bufferLen)
      n = bufferLen;
   int i;
   for (i = 0; i < n; i++) {
      bufferWhen[i] = mEnv[i].GetT() - mOffset;
      bufferValue[i] = mEnv[i].GetVal();
   }
}

// Private methods

// We no longer tolerate multiple envelope control points at the exact
// same t; the behavior can be well-defined, but it is still incorrect
// in that it vastly complicates paste operations behaving as a user
// reasonably expects.  The most common problem occurs pasting an
// envelope into another track; the boundary behavior causes the
// t=insert_point envelope level of the insertee to apply to sample 0
// of the inserted sample, causing a pop.  This most visibly manifests
// itself in undo and mixing when a v=1.0 sample magically shows
// up at boundaries causing a pop.

// Although this renders the name a slight misnomer, a duplicate
// 'replaces' the current control point.

/** @brief Add a control point to the envelope
 *
 * @param when the time in seconds when the envelope point should be created.
 * @param value the envelope value to use at the given point.
 * @return the index of the NEW envelope point within array of envelope points.
 */
int Envelope::InsertOrReplaceRelative(double when, double value)
{
#if defined(__WXDEBUG__)
   // in debug builds, do a spot of argument checking
   if(when > mTrackLen + 0.0000001)
   {
      wxString msg;
      msg = wxString::Format(wxT("when %.20f mTrackLen %.20f diff %.20f"), when, mTrackLen, when-mTrackLen);
      wxASSERT_MSG(when <= (mTrackLen), msg);
   }
   if(when < 0)
   {
      wxString msg;
      msg = wxString::Format(wxT("when %.20f mTrackLen %.20f"), when, mTrackLen);
      wxASSERT_MSG(when >= 0, msg);
   }
#endif

   int len = mEnv.size();

   if (len && when < 0.0)
      return 0;
   if ((len > 1) && when > mTrackLen)
      return len - 1;

   if (when < 0.0)
      when = 0.0;
   if ((len>1) && when > mTrackLen)
      when = mTrackLen;

   int i = 0;

   while (i < len && when > mEnv[i].GetT())
      i++;

   if(i < len && when == mEnv[i].GetT())
     // modify existing
     mEnv[i].SetVal( this, value );
   else {
     // Add NEW
     EnvPoint e{ when, value };
     if (i < len) {
        Insert(i, e);
     } else {
        mEnv.push_back(e);
     }
   }
   return i;
}

std::pair<int, int> Envelope::EqualRange( double when, double sampleTime ) const
{
   // Find range of envelope points matching the given time coordinate
   // (within an interval of length sampleTime)
   // by binary search; if empty, it still indicates where to
   // insert.
   const auto tolerance = sampleTime / 2;
   auto begin = mEnv.begin();
   auto end = mEnv.end();
   auto first = std::lower_bound(
      begin, end,
      EnvPoint{ when - tolerance, 0.0 },
      []( const EnvPoint &point1, const EnvPoint &point2 )
         { return point1.GetT() < point2.GetT(); }
   );
   auto after = first;
   while ( after != end && after->GetT() <= when + tolerance )
      ++after;
   return { first - begin, after - begin };
}

// Control

void Envelope::SetOffset(double newOffset)
// NOFAIL-GUARANTEE
{
   mOffset = newOffset;
}

void Envelope::SetTrackLen(double trackLen)
// NOFAIL-GUARANTEE
{
   // Preserve the right-side limit at trackLen.
   bool needPoint = ( trackLen < mTrackLen );
   double value;
   if ( needPoint )
      value = GetValueRelative( trackLen );

   mTrackLen = trackLen;
   int newLen = EqualRange( trackLen, 0 ).second;
   mEnv.resize( newLen );

   if ( needPoint )
      AddPointAtEnd( mTrackLen, value );
}

void Envelope::RescaleTimes( double newLength )
// NOFAIL-GUARANTEE
{
   if ( mTrackLen == 0 ) {
      for ( auto &point : mEnv )
         point.SetT( 0 );
   }
   else {
      auto ratio = newLength / mTrackLen;
      for ( auto &point : mEnv )
         point.SetT( point.GetT() * ratio );
   }
   mTrackLen = newLength;
}

// Accessors
double Envelope::GetValue(double t) const
{
   // t is absolute time
   double temp;

   GetValues(&temp, 1, t, 1.0);
   return temp;
}

double Envelope::GetValueRelative(double t) const
{
   double temp;

   GetValuesRelative(&temp, 1, t, 1.0);
   return temp;
}

// relative time
/// @param Lo returns last index at or before this time, maybe -1
/// @param Hi returns first index after this time, maybe past the end
void Envelope::BinarySearchForTime( int &Lo, int &Hi, double t ) const
{
   // Optimizations for the usual pattern of repeated calls with
   // small increases of t.
   {
      if (mSearchGuess >= 0 && mSearchGuess < mEnv.size()) {
         if (t >= mEnv[mSearchGuess].GetT() &&
             (1 + mSearchGuess == mEnv.size() ||
              t < mEnv[1 + mSearchGuess].GetT())) {
            Lo = mSearchGuess;
            Hi = 1 + mSearchGuess;
            return;
         }
      }

      ++mSearchGuess;
      if (mSearchGuess >= 0 && mSearchGuess < mEnv.size()) {
         if (t >= mEnv[mSearchGuess].GetT() &&
             (1 + mSearchGuess == mEnv.size() ||
              t < mEnv[1 + mSearchGuess].GetT())) {
            Lo = mSearchGuess;
            Hi = 1 + mSearchGuess;
            return;
         }
      }
   }

   Lo = -1;
   Hi = mEnv.size();

   // Invariants:  Lo is not less than -1, Hi not more than size
   while (Hi > (Lo + 1)) {
      int mid = (Lo + Hi) / 2;
      // mid must be strictly between Lo and Hi, therefore a valid index
      if (t < mEnv[mid].GetT())
         Hi = mid;
      else
         Lo = mid;
   }
   wxASSERT( Hi == ( Lo+1 ));

   mSearchGuess = Lo;
}

/// GetInterpolationStartValueAtPoint() is used to select either the
/// envelope value or its log depending on whether we are doing linear
/// or log interpolation.
/// @param iPoint index in env array to look at.
/// @return value there, or its (safe) log10.
double Envelope::GetInterpolationStartValueAtPoint( int iPoint ) const
{
   double v = mEnv[ iPoint ].GetVal();
   if( !mDB )
      return v;
   else
      return log10(v);
}

void Envelope::GetValues(double *buffer, int bufferLen,
                         double t0, double tstep) const
{
   // Convert t0 from absolute to clip-relative time
   t0 -= mOffset;
   GetValuesRelative( buffer, bufferLen, t0, tstep);
}

void Envelope::GetValuesRelative(double *buffer, int bufferLen,
                         double t0, double tstep) const
{
   // JC: If bufferLen ==0 we have probably just allocated a zero sized buffer.
   // wxASSERT( bufferLen > 0 );

   int len = mEnv.size();

   double t = t0;
   double tprev, vprev, tnext = 0, vnext, vstep = 0;

   for (int b = 0; b < bufferLen; b++) {

      // Get easiest cases out the way first...
      // IF empty envelope THEN default value
      if (len <= 0) {
         buffer[b] = mDefaultValue;
         t += tstep;
         continue;
      }
      // IF before envelope THEN first value
      if (t <= mEnv[0].GetT()) {
         buffer[b] = mEnv[0].GetVal();
         t += tstep;
         continue;
      }
      // IF after envelope THEN last value
      if (t >= mEnv[len - 1].GetT()) {
         buffer[b] = mEnv[len - 1].GetVal();
         t += tstep;
         continue;
      }

      if (b == 0 || t > tnext) {

         // We're beyond our tnext, so find the next one.
         // Don't just increment lo or hi because we might
         // be zoomed far out and that could be a large number of
         // points to move over.  That's why we binary search.

         int lo,hi;
         BinarySearchForTime( lo, hi, t );
         // mEnv[0] is before t because of eliminations above, therefore lo >= 0
         // mEnv[len - 1] is after t, therefore hi <= len - 1
         tprev = mEnv[lo].GetT();
         tnext = mEnv[hi].GetT();

         vprev = GetInterpolationStartValueAtPoint( lo );
         vnext = GetInterpolationStartValueAtPoint( hi );

         // Interpolate, either linear or log depending on mDB.
         double dt = (tnext - tprev);
         double to = t - tprev;
         double v;
         if (dt > 0.0)
         {
            v = (vprev * (dt - to) + vnext * to) / dt;
            vstep = (vnext - vprev) * tstep / dt;
         }
         else
         {
            v = vnext;
            vstep = 0.0;
         }

         // An adjustment if logarithmic scale.
         if( mDB )
         {
            v = pow(10.0, v);
            vstep = pow( 10.0, vstep );
         }

         buffer[b] = v;
      } else {
         if (mDB){
            buffer[b] = buffer[b - 1] * vstep;
         }else{
            buffer[b] = buffer[b - 1] + vstep;
         }
      }

      t += tstep;
   }
}

void Envelope::GetValues
   (double *buffer, int bufferLen, int leftOffset, const ZoomInfo &zoomInfo) const
{
   for (int xx = 0; xx < bufferLen; ++xx)
      buffer[xx] = GetValue(zoomInfo.PositionToTime(xx, -leftOffset));
}

// relative time
int Envelope::NumberOfPointsAfter(double t) const
{
   int lo,hi;
   BinarySearchForTime( lo, hi, t );

   return mEnv.size() - hi;
}

// relative time
double Envelope::NextPointAfter(double t) const
{
   int lo,hi;
   BinarySearchForTime( lo, hi, t );
   if (hi >= mEnv.size())
      return t;
   else
      return mEnv[hi].GetT();
}

double Envelope::Average( double t0, double t1 ) const
{
  if( t0 == t1 )
    return GetValue( t0 );
  else
    return Integral( t0, t1 ) / (t1 - t0);
}

double Envelope::AverageOfInverse( double t0, double t1 ) const
{
  if( t0 == t1 )
    return 1.0 / GetValue( t0 );
  else
    return IntegralOfInverse( t0, t1 ) / (t1 - t0);
}

//
// Integration and debugging functions
//
// The functions below are used by the TimeTrack and possibly for
// other debugging.  They do not affect normal amplitude envelopes
// for waveforms, nor frequency envelopes for equalization.
// The 'Average' function also uses 'Integral'.
//

// A few helper functions to make the code below more readable.
static double InterpolatePoints(double y1, double y2, double factor, bool logarithmic)
{
   if(logarithmic)
      // you can use any base you want, it doesn't change the result
      return exp(log(y1) * (1.0 - factor) + log(y2) * factor);
   else
      return y1 * (1.0 - factor) + y2 * factor;
}
static double IntegrateInterpolated(double y1, double y2, double time, bool logarithmic)
{
   // Calculates: integral(interpolate(y1, y2, x), x = 0 .. time)
   // Integrating logarithmic interpolated segments is surprisingly simple. You can check this formula here:
   // http://www.wolframalpha.com/input/?i=integrate+10%5E%28log10%28y1%29*%28T-x%29%2FT%2Blog10%28y2%29*x%2FT%29+from+0+to+T
   // Again, the base you use for interpolation is irrelevant, the formula below should always use the natural
   // logarithm (i.e. 'log' in C/C++). If the denominator is too small, it's better to use linear interpolation
   // because the rounding errors would otherwise get too large. The threshold value is 1.0e-5 because at that
   // point the rounding errors become larger than the difference between linear and logarithmic (I tested this in Octave).
   if(logarithmic)
   {
      double l = log(y1 / y2);
      if(fabs(l) < 1.0e-5) // fall back to linear interpolation
         return (y1 + y2) * 0.5 * time;
      return (y1 - y2) / l * time;
   }
   else
   {
      return (y1 + y2) * 0.5 * time;
   }
}
static double IntegrateInverseInterpolated(double y1, double y2, double time, bool logarithmic)
{
   // Calculates: integral(1 / interpolate(y1, y2, x), x = 0 .. time)
   // This one is a bit harder. Linear:
   // http://www.wolframalpha.com/input/?i=integrate+1%2F%28y1*%28T-x%29%2FT%2By2*x%2FT%29+from+0+to+T
   // Logarithmic:
   // http://www.wolframalpha.com/input/?i=integrate+1%2F%2810%5E%28log10%28y1%29*%28T-x%29%2FT%2Blog10%28y2%29*x%2FT%29%29+from+0+to+T
   // Here both cases need a special case for y1 == y2. The threshold is 1.0e5 again, this is still the
   // best value in both cases.
   double l = log(y1 / y2);
   if(fabs(l) < 1.0e-5) // fall back to average
      return 2.0 / (y1 + y2) * time;
   if(logarithmic)
      return (y1 - y2) / (l * y1 * y2) * time;
   else
      return l / (y1 - y2) * time;
}
static double SolveIntegrateInverseInterpolated(double y1, double y2, double time, double area, bool logarithmic)
{
   // Calculates: solve (integral(1 / interpolate(y1, y2, x), x = 0 .. res) = area) for res
   // Don't try to derive these formulas by hand :). The threshold is 1.0e5 again.
   double a = area / time, res;
   if(logarithmic)
   {
      double l = log(y1 / y2);
      if(fabs(l) < 1.0e-5) // fall back to average
         res = a * (y1 + y2) * 0.5;
      else if(1.0 + a * y1 * l <= 0.0)
         res = 1.0;
      else
         res = log1p(a * y1 * l) / l;
   }
   else
   {
      if(fabs(y2 - y1) < 1.0e-5) // fall back to average
         res = a * (y1 + y2) * 0.5;
      else
         res = y1 * expm1(a * (y2 - y1)) / (y2 - y1);
   }
   return std::max(0.0, std::min(1.0, res)) * time;
}

// We should be able to write a very efficient memoizer for this
// but make sure it gets reset when the envelope is changed.
double Envelope::Integral( double t0, double t1 ) const
{
   if(t0 == t1)
      return 0.0;
   if(t0 > t1)
   {
      return -Integral(t1, t0); // this makes more sense than returning the default value
   }

   unsigned int count = mEnv.size();
   if(count == 0) // 'empty' envelope
      return (t1 - t0) * mDefaultValue;

   t0 -= mOffset;
   t1 -= mOffset;

   double total = 0.0, lastT, lastVal;
   unsigned int i; // this is the next point to check
   if(t0 < mEnv[0].GetT()) // t0 preceding the first point
   {
      if(t1 <= mEnv[0].GetT())
         return (t1 - t0) * mEnv[0].GetVal();
      i = 1;
      lastT = mEnv[0].GetT();
      lastVal = mEnv[0].GetVal();
      total += (lastT - t0) * lastVal;
   }
   else if(t0 >= mEnv[count - 1].GetT()) // t0 at or following the last point
   {
      return (t1 - t0) * mEnv[count - 1].GetVal();
   }
   else // t0 enclosed by points
   {
      // Skip any points that come before t0 using binary search
      int lo, hi;
      BinarySearchForTime(lo, hi, t0);
      lastVal = InterpolatePoints(mEnv[lo].GetVal(), mEnv[hi].GetVal(), (t0 - mEnv[lo].GetT()) / (mEnv[hi].GetT() - mEnv[lo].GetT()), mDB);
      lastT = t0;
      i = hi; // the point immediately after t0.
   }

   // loop through the rest of the envelope points until we get to t1
   while (1)
   {
      if(i >= count) // the requested range extends beyond the last point
      {
         return total + (t1 - lastT) * lastVal;
      }
      else if(mEnv[i].GetT() >= t1) // this point follows the end of the range
      {
         double thisVal = InterpolatePoints(mEnv[i - 1].GetVal(), mEnv[i].GetVal(), (t1 - mEnv[i - 1].GetT()) / (mEnv[i].GetT() - mEnv[i - 1].GetT()), mDB);
         return total + IntegrateInterpolated(lastVal, thisVal, t1 - lastT, mDB);
      }
      else // this point precedes the end of the range
      {
         total += IntegrateInterpolated(lastVal, mEnv[i].GetVal(), mEnv[i].GetT() - lastT, mDB);
         lastT = mEnv[i].GetT();
         lastVal = mEnv[i].GetVal();
         i++;
      }
   }
}

double Envelope::IntegralOfInverse( double t0, double t1 ) const
{
   if(t0 == t1)
      return 0.0;
   if(t0 > t1)
   {
      return -IntegralOfInverse(t1, t0); // this makes more sense than returning the default value
   }

   unsigned int count = mEnv.size();
   if(count == 0) // 'empty' envelope
      return (t1 - t0) / mDefaultValue;

   t0 -= mOffset;
   t1 -= mOffset;

   double total = 0.0, lastT, lastVal;
   unsigned int i; // this is the next point to check
   if(t0 < mEnv[0].GetT()) // t0 preceding the first point
   {
      if(t1 <= mEnv[0].GetT())
         return (t1 - t0) / mEnv[0].GetVal();
      i = 1;
      lastT = mEnv[0].GetT();
      lastVal = mEnv[0].GetVal();
      total += (lastT - t0) / lastVal;
   }
   else if(t0 >= mEnv[count - 1].GetT()) // t0 at or following the last point
   {
      return (t1 - t0) / mEnv[count - 1].GetVal();
   }
   else // t0 enclosed by points
   {
      // Skip any points that come before t0 using binary search
      int lo, hi;
      BinarySearchForTime(lo, hi, t0);
      lastVal = InterpolatePoints(mEnv[lo].GetVal(), mEnv[hi].GetVal(), (t0 - mEnv[lo].GetT()) / (mEnv[hi].GetT() - mEnv[lo].GetT()), mDB);
      lastT = t0;
      i = hi; // the point immediately after t0.
   }

   // loop through the rest of the envelope points until we get to t1
   while (1)
   {
      if(i >= count) // the requested range extends beyond the last point
      {
         return total + (t1 - lastT) / lastVal;
      }
      else if(mEnv[i].GetT() >= t1) // this point follows the end of the range
      {
         double thisVal = InterpolatePoints(mEnv[i - 1].GetVal(), mEnv[i].GetVal(), (t1 - mEnv[i - 1].GetT()) / (mEnv[i].GetT() - mEnv[i - 1].GetT()), mDB);
         return total + IntegrateInverseInterpolated(lastVal, thisVal, t1 - lastT, mDB);
      }
      else // this point precedes the end of the range
      {
         total += IntegrateInverseInterpolated(lastVal, mEnv[i].GetVal(), mEnv[i].GetT() - lastT, mDB);
         lastT = mEnv[i].GetT();
         lastVal = mEnv[i].GetVal();
         i++;
      }
   }
}

double Envelope::SolveIntegralOfInverse( double t0, double area ) const
{
   if(area == 0.0)
      return t0;

   unsigned int count = mEnv.size();
   if(count == 0) // 'empty' envelope
      return t0 + area * mDefaultValue;

   // Correct for offset!
   t0 -= mOffset;
   return mOffset + [&] {
      // Now we can safely assume t0 is relative time!
      double lastT, lastVal;
      int i; // this is the next point to check
      if(t0 < mEnv[0].GetT()) // t0 preceding the first point
      {
         if (area < 0) {
            return t0 + area * mEnv[0].GetVal();
         }
         else {
            i = 1;
            lastT = mEnv[0].GetT();
            lastVal = mEnv[0].GetVal();
            double added = (lastT - t0) / lastVal;
            if(added >= area)
               return t0 + area * mEnv[0].GetVal();
            area -= added;
         }
      }
      else if(t0 >= mEnv[count - 1].GetT()) // t0 at or following the last point
      {
         if (area < 0) {
            i = count - 2;
            lastT = mEnv[count - 1].GetT();
            lastVal = mEnv[count - 1].GetVal();
            double added = (lastT - t0) / lastVal; // negative
            if(added <= area)
               return t0 + area * mEnv[count - 1].GetVal();
            area -= added;
         }
         else {
            return t0 + area * mEnv[count - 1].GetVal();
         }
      }
      else // t0 enclosed by points
      {
         // Skip any points that come before t0 using binary search
         int lo, hi;
         BinarySearchForTime(lo, hi, t0);
         lastVal = InterpolatePoints(mEnv[lo].GetVal(), mEnv[hi].GetVal(), (t0 - mEnv[lo].GetT()) / (mEnv[hi].GetT() - mEnv[lo].GetT()), mDB);
         lastT = t0;
         if (area < 0)
            i = lo;
         else
            i = hi; // the point immediately after t0.
      }

      if (area < 0) {
         // loop BACKWARDS through the rest of the envelope points until we get to t1
         // (which is less than t0)
         while (1)
         {
            if(i < 0) // the requested range extends beyond the leftmost point
            {
               return lastT + area * lastVal;
            }
            else
            {
               double added =
                  -IntegrateInverseInterpolated(mEnv[i].GetVal(), lastVal, lastT - mEnv[i].GetT(), mDB);
               if(added <= area)
                  return lastT - SolveIntegrateInverseInterpolated(lastVal, mEnv[i].GetVal(), lastT - mEnv[i].GetT(), -area, mDB);
               area -= added;
               lastT = mEnv[i].GetT();
               lastVal = mEnv[i].GetVal();
               --i;
            }
         }
      }
      else {
         // loop through the rest of the envelope points until we get to t1
         while (1)
         {
            if(i >= count) // the requested range extends beyond the last point
            {
               return lastT + area * lastVal;
            }
            else
            {
               double added = IntegrateInverseInterpolated(lastVal, mEnv[i].GetVal(), mEnv[i].GetT() - lastT, mDB);
               if(added >= area)
                  return lastT + SolveIntegrateInverseInterpolated(lastVal, mEnv[i].GetVal(), mEnv[i].GetT() - lastT, area, mDB);
               area -= added;
               lastT = mEnv[i].GetT();
               lastVal = mEnv[i].GetVal();
               i++;
            }
         }
      }
   }();
}

void Envelope::print() const
{
   for( unsigned int i = 0; i < mEnv.size(); i++ )
      printf( "(%.2f, %.2f)\n", mEnv[i].GetT(), mEnv[i].GetVal() );
}

static void checkResult( int n, double a, double b )
{
   if( (a-b > 0 ? a-b : b-a) > 0.0000001 )
   {
      printf( "Envelope:  Result #%d is: %f, should be %f\n", n, a, b );
      //exit( -1 );
   }
}

void Envelope::testMe()
{
   double t0=0, t1=0;

   SetExponential(false);

   Flatten(0.5);
   checkResult( 1, Integral(0.0,100.0), 50);
   checkResult( 2, Integral(-10.0,10.0), 10);

   Flatten(0.5);
   checkResult( 3, Integral(0.0,100.0), 50);
   checkResult( 4, Integral(-10.0,10.0), 10);
   checkResult( 5, Integral(-20.0,-10.0), 5);

   Flatten(0.5);
   InsertOrReplaceRelative( 5.0, 0.5 );
   checkResult( 6, Integral(0.0,100.0), 50);
   checkResult( 7, Integral(-10.0,10.0), 10);

   Flatten(0.0);
   InsertOrReplaceRelative( 0.0, 0.0 );
   InsertOrReplaceRelative( 5.0, 1.0 );
   InsertOrReplaceRelative( 10.0, 0.0 );
   t0 = 10.0 - .1;
   t1 = 10.0 + .1;
   double result = Integral(0.0,t1);
   double resulta = Integral(0.0,t0);
   double resultb = Integral(t0,t1);
   // Integrals should be additive
   checkResult( 8, result - resulta - resultb, 0);

   Flatten(0.0);
   InsertOrReplaceRelative( 0.0, 0.0 );
   InsertOrReplaceRelative( 5.0, 1.0 );
   InsertOrReplaceRelative( 10.0, 0.0 );
   t0 = 10.0 - .1;
   t1 = 10.0 + .1;
   checkResult( 9, Integral(0.0,t1), 5);
   checkResult( 10, Integral(0.0,t0), 4.999);
   checkResult( 11, Integral(t0,t1), .001);

   mEnv.clear();
   InsertOrReplaceRelative( 0.0, 0.0 );
   InsertOrReplaceRelative( 5.0, 1.0 );
   InsertOrReplaceRelative( 10.0, 0.0 );
   checkResult( 12, NumberOfPointsAfter( -1 ), 3 );
   checkResult( 13, NumberOfPointsAfter( 0 ), 2 );
   checkResult( 14, NumberOfPointsAfter( 1 ), 2 );
   checkResult( 15, NumberOfPointsAfter( 5 ), 1 );
   checkResult( 16, NumberOfPointsAfter( 7 ), 1 );
   checkResult( 17, NumberOfPointsAfter( 10 ), 0 );
   checkResult( 18, NextPointAfter( 0 ), 5 );
   checkResult( 19, NextPointAfter( 5 ), 10 );
}

EnvelopeEditor::EnvelopeEditor(Envelope &envelope, bool mirrored)
   : mEnvelope(envelope)
   , mMirrored(mirrored)
   , mContourOffset(-1)
   // , mInitialVal(-1.0)
   // , mInitialY(-1)
   , mUpper(false)
   , mButton(wxMOUSE_BTN_NONE)
   , mDirty(false)
{
}

EnvelopeEditor::~EnvelopeEditor()
{
}
