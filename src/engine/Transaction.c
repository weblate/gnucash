/********************************************************************\
 * Transaction.c -- the transaction data structure                  *
 * Copyright (C) 1997 Robin D. Clark                                *
 * Copyright (C) 1997, 1998 Linas Vepstas                           *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, write to the Free Software      *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.        *
 *                                                                  *
 *   Author: Rob Clark                                              *
 * Internet: rclark@cs.hmc.edu                                      *
 *  Address: 609 8th Street                                         *
 *           Huntington Beach, CA 92648-4632                        *
\********************************************************************/

#include <assert.h>
#include <string.h>

#include "config.h"

#include "Account.h"
#include "AccountP.h"
#include "Group.h"
#include "Transaction.h"
#include "TransactionP.h"
#include "TransLog.h"
#include "util.h"
#include "date.h"


/* 
 * If the "force_double_entry" flag determines how 
 * the splits in a transaction will be balanced. 
 *
 * The following values have significance:
 * 0 -- anything goes
 * 1 -- The sum of all splits in a transaction will be'
 *      forced to be zero, even if this requires the
 *      creation of additonal splits.  Note that a split
 *      whose value is zero (e.g. a stock price) can exist
 *      by itself. Otherwise, all splits must come in at 
 *      least pairs.
 * 2 -- splits without parents will be forced into a
 *      lost & found account.  (Not implemented)
 */
int force_double_entry = 0;

/* bit-field flags for controlling transaction commits */
#define BEGIN_EDIT 0x1
#define DEFER_REBALANCE 0x2

/********************************************************************\
 * Because I can't use C++ for this project, doesn't mean that I    *
 * can't pretend too!  These functions perform actions on the       *
 * Transaction data structure, in order to encapsulate the          *
 * knowledge of the internals of the Transaction in one file.       *
\********************************************************************/


/********************************************************************\
 * xaccInitSplit
 * Initialize a splitaction structure
\********************************************************************/

void
xaccInitSplit( Split * split )
  {
  
  /* fill in some sane defaults */
  split->acc         = NULL;
  split->parent      = NULL;
  
  split->action      = strdup("");
  split->memo        = strdup("");
  split->docref      = strdup("");
  split->reconciled  = NREC;
  split->damount     = 0.0;
  split->share_price = 1.0;

  split->date_reconciled.tv_sec = 0;
  split->date_reconciled.tv_nsec = 0;

  split->balance             = 0.0;
  split->cleared_balance     = 0.0;
  split->reconciled_balance  = 0.0;
  split->share_balance             = 0.0;
  split->share_cleared_balance     = 0.0;
  split->share_reconciled_balance  = 0.0;

  }

/********************************************************************\
\********************************************************************/
Split *
xaccMallocSplit( void )
  {
  Split *split = (Split *)_malloc(sizeof(Split));
  xaccInitSplit (split);
  return split;
  }

/********************************************************************\
\********************************************************************/

void
xaccFreeSplit( Split *split )
  {
  if (!split) return;

  if (split->memo) free (split->memo);
  if (split->action) free (split->action);
  if (split->docref) free (split->docref);

  /* just in case someone looks up freed memory ... */
  split->memo        = 0x0;
  split->action      = 0x0;
  split->docref      = 0x0;
  split->reconciled  = NREC;
  split->damount     = 0.0;
  split->share_price = 1.0;
  split->parent      = NULL;
  split->acc         = NULL;

  split->date_reconciled.tv_sec = 0;
  split->date_reconciled.tv_nsec = 0;

  _free(split);
}

/********************************************************************\
\********************************************************************/

#define MARK_SPLIT(split) {			\
   Account *acc = (Account *) ((split)->acc);	\
   if (acc) acc->changed = 1;			\
}

static void
MarkChanged (Transaction *trans)
{
   if (trans->splits) {
      int i=0;
      while (trans->splits[i]) {
         MARK_SPLIT (trans->splits[i]);
         i++;
      }
   }
}

/********************************************************************\
\********************************************************************/

int
xaccCountSplits (Split **tarray)
{
   Split *split;
   int nsplit = 0;

   if (!tarray) return 0;

   split = tarray[0];
   while (split) {
      nsplit ++;
      split = tarray[nsplit];
   }
   return nsplit;
}

/********************************************************************\
\********************************************************************/

void xaccSplitSetSharePriceAndAmount (Split *s, double price, double amt)
{
   MARK_SPLIT(s);
   s -> share_price = price;
   s -> damount = amt;

   /* force double entry to always balance */
   xaccSplitRebalance (s);
}

void xaccSplitSetSharePrice (Split *s, double amt)
{
   MARK_SPLIT(s);
   s -> share_price = amt;

   /* force double entry to always balance */
   xaccSplitRebalance (s);
}

void xaccSplitSetShareAmount (Split *s, double amt)
{
   MARK_SPLIT(s);
   s -> damount = amt;

   /* force double entry to always balance */
   xaccSplitRebalance (s);
}

void xaccSplitSetValue (Split *s, double amt)
{
   MARK_SPLIT(s);
   /* remember, damount is actually share price */
   s -> damount = amt / (s->share_price);

   /* force double entry to always balance */
   xaccSplitRebalance (s);
}

/********************************************************************\
\********************************************************************/

double xaccSplitGetBalance (Split *s) 
{
   if (!s) return 0.0;
   return s->balance;
}

double xaccSplitGetClearedBalance (Split *s) 
{
   if (!s) return 0.0;
   return s->cleared_balance;
}

double xaccSplitGetReconciledBalance (Split *s) 
{
   if (!s) return 0.0;
   return s->reconciled_balance;
}

double xaccSplitGetShareBalance (Split *s) 
{
   if (!s) return 0.0;
   return s->share_balance;
}

/********************************************************************\
 * xaccInitTransaction
 * Initialize a transaction structure
\********************************************************************/

void
xaccInitTransaction( Transaction * trans )
  {
  Split *split;
  
  /* fill in some sane defaults */
  trans->num         = strdup("");
  trans->description = strdup("");
  trans->docref      = strdup("");

  trans->splits    = (Split **) _malloc (3* sizeof (Split *));

  /* create a single split only.  As soon as the balance becomes
   * non-zero, additional splits will get created. 
   */
  split = xaccMallocSplit ();
  split->parent = trans;
  trans->splits[0] = split;
  trans->splits[1] = NULL;

  trans->date_entered.tv_sec = 0;
  trans->date_entered.tv_nsec = 0;

  trans->date_posted.tv_sec = 0;
  trans->date_posted.tv_nsec = 0;

  trans->open        = 0;
  }

/********************************************************************\
\********************************************************************/

Transaction *
xaccMallocTransaction( void )
  {
  Transaction *trans = (Transaction *)_malloc(sizeof(Transaction));
  xaccInitTransaction (trans);
  return trans;
  }

/********************************************************************\
\********************************************************************/

void
xaccFreeTransaction( Transaction *trans )
  {
  int i;
  Split *s;

  if (!trans) return;

  /* free up the destination splits */
  i = 0;
  s = trans->splits[i];
  while (s) {
    xaccFreeSplit (s);
    i++;
    s = trans->splits[i];
  }

  _free (trans->splits);

  /* free up transaction strings */
  if (trans->num) free (trans->num);
  if (trans->description) free (trans->description);
  if (trans->docref) free (trans->docref);

  /* just in case someone looks up freed memory ... */
  trans->num         = 0x0;
  trans->description = 0x0;
  trans->docref      = 0x0;

  trans->date_entered.tv_sec = 0;
  trans->date_entered.tv_nsec = 0;

  trans->date_posted.tv_sec = 0;
  trans->date_posted.tv_nsec = 0;

  trans->open = 0;

  _free(trans);
}

/********************************************************************\
\********************************************************************/

void
xaccSplitSetBaseValue (Split *s, double value, char * base_currency)
{
   if (!s) return;

   /* ahh -- stupid users may not want or use the double entry 
    * features of this engine.  So, in particular, there
    * may be the occasional split without a parent account. 
    * Well, that's ok,  we'll just go with the flow. 
    */
   if (!(s->acc)) {
      if (force_double_entry) {
         assert (s->acc);
      } else { 
         s -> damount = (value / (s->share_price));   
         return;
      }
   }

   /* be more precise -- the value depends on the curency 
    * we want it expressed in.
    */
   if (!safe_strcmp(s->acc->currency, base_currency)) {
      s -> damount = (value / (s->share_price));   
   } else 
   if (!safe_strcmp(s->acc->security, base_currency)) {
      s -> damount = value;   
   } else 
   if ((0x0==base_currency) && (0 == force_double_entry)) {
      s -> damount = (value / (s->share_price));   
   } else 
   {
      printf ("Error: xaccSplitSetBaseValue(): "
              " inappropriate base currency %s "
              " given split currency=%s and security=%s\n",
              base_currency, s->acc->currency, s->acc->security);
      return;
   }
}


double
xaccSplitGetBaseValue (Split *s, char * base_currency)
{
   double value;
   if (!s) return 0.0;

   /* ahh -- stupid users may not want or use the double entry 
    * features of this engine.  So, in particular, there
    * may be the occasional split without a parent account. 
    * Well, that's ok,  we'll just go with the flow. 
    */
   if (!(s->acc)) {
      if (force_double_entry) {
         assert (s->acc);
      } else { 
         value = s->damount * s->share_price;   
         return value;
      }
   }

   /* be more precise -- the value depends on the curency 
    * we want it expressed in.
    */
   if (!safe_strcmp(s->acc->currency, base_currency)) {
      value = s->damount * s->share_price;   
   } else 
   if (!safe_strcmp(s->acc->security, base_currency)) {
      value = s->damount;   
   } else 
   if ((0x0==base_currency) && (0 == force_double_entry)) {
      value = s->damount * s->share_price;   
   } else 
   {
      printf ("Error: xaccSplitGetBaseValue(): "
              " inappropriate base currency %s "
              " given split currency=%s and security=%s\n",
              base_currency, s->acc->currency, s->acc->security);
      return 0.0;
   }
   return value;
}

/********************************************************************\
\********************************************************************/


static double
ComputeValue (Split **sarray, Split * skip_me, char * base_currency)
{
   Split *s;
   int i=0;
   double value = 0.0;

   s = sarray[0];
   while (s) {
      if (s != skip_me) {
         /* ahh -- stupid users may not want or use the double entry 
          * features of this engine.  So, in particular, there
          * may be the occasional split without a parent account. 
          * Well, that's ok,  we'll just go with the flow. 
          */
         if (!(s->acc)) {
            if (force_double_entry) {
               assert (s->acc);
            } else { 
               value += s->damount * s->share_price;   
            }
         } else 
         if ((0x0 == base_currency) && (0 == force_double_entry)) {
            value += s->damount * s->share_price;   
         } else {

            /* OK, we've got a parent account, we've got currency, 
             * lets behave like profesionals now, instead of the
             * shenanigans above.
             */
            if (!safe_strcmp(s->acc->currency, base_currency)) {
               value += s->share_price * s->damount;
            } else 
            if (!safe_strcmp(s->acc->security, base_currency)) {
               value += s->damount;
            } else {
               printf ("Internal Error: ComputeValue(): "
                       " inconsistent currencies \n");
               assert (0);
            }
         }
      }
      i++; s = sarray [i];
   }

   return value;
}

/* hack alert -- the algorithm used in this rebalance routine
 * is less than intuitive, and could use some write-up.  
 * Maybe it does indeed do the right thing, but that is
 * not at all obvious.
 */

void
xaccTransRebalance (Transaction * trans)
{
  xaccSplitRebalance (trans->splits[0]);
}

void
xaccSplitRebalance (Split *split)
{
  Transaction *trans;
  Split *s;
  int i = 0;
  double value = 0.0;
  char *base_currency=0x0;
  char *ra=0x0, *rb =0x0;

  trans = split->parent;

  /* We might have gotten here if someone is manipulating
   * a split that has not yet been inserted in a transaction.
   * Rather than punishing them with an assert, lets just
   * quietly return. 
   */
  if (!trans) return;

  if (DEFER_REBALANCE & (trans->open)) return;
  if (split->acc) {
    if (ACC_DEFER_REBALANCE & (split->acc->open)) return;
    assert (trans->splits);
    assert (trans->splits[0]);
  
    /* lets find out if we are dealing with multiple currencies,
     * and which one(s) all of the splits have in common.  */
    ra = split->acc->currency;
    rb = split->acc->security;
    if (rb && (0x0==rb[0])) rb = 0x0;
  
    i=0; s = trans->splits[0];
    while (s) {
      char *sa, *sb;
  
      /* ahh -- stupid users may not want or use the double entry 
       * features of this engine.  So, in particular, there
       * may be the occasional split without a parent account. 
       * Well, that's ok,  we'll just go with the flow. 
       */
      if (force_double_entry) {
         assert (s->acc);
      } else {
         i++; s=trans->splits[i]; continue;
      }
  
      sa = s->acc->currency;
      sb = s->acc->security;
      if (sb && (0x0==sb[0])) sb = 0x0;
  
      if (ra && rb) {
         int aa = safe_strcmp (ra,sa);
         int ab = safe_strcmp (ra,sb);
         int ba = safe_strcmp (rb,sa);
         int bb = safe_strcmp (rb,sb);
         if ( (!aa) && bb) rb = 0x0;
         else
         if ( (!ab) && ba) rb = 0x0;
         else
         if ( (!ba) && ab) ra = 0x0;
         else
         if ( (!bb) && aa) ra = 0x0;
         else
         if ( aa && bb && ab && ba ) { ra=0x0; rb=0x0; }
  
         if (!ra) { ra=rb; rb=0x0; }
      } 
      else
      if (ra && !rb) {
         int aa = safe_strcmp (ra,sa);
         int ab = safe_strcmp (ra,sb);
         if ( aa && ab )  ra= 0x0;
      }
  
    if ((!ra) && (!rb)) {
        printf ("Internal Error: SplitRebalance(): "
                " no common split currencies \n");
        printf ("\tbase acc=%s cur=%s base_sec=%s\n"
                "\tacc=%s scur=%s ssec=%s \n", 
            split->acc->accountName, split->acc->currency, split->acc->security,
            s->acc->accountName, s->acc->currency, s->acc->security );
        assert (0);
        return;
      }
      i++; s = trans->splits[i];
    }
  } else {
    assert (trans->splits);
    assert (trans->splits[0]);
  }

  base_currency = ra;

  if (split == trans->splits[0]) {
    /* The indicated split is the source split.
     * Pick a destination split (by default, 
     * the first destination split), and force 
     * the total on it. 
     */

    s = trans->splits[1];
    if (s) {
      /* the new value of the destination split will be the result.  */
      value = ComputeValue (trans->splits, s, base_currency);
      xaccSplitSetBaseValue (s, -value, base_currency);
      MARK_SPLIT (s);
      xaccAccountRecomputeBalance (s->acc); 

    } else {
      /* There are no destination splits !! 
       * Either this is allowed, in which case 
       * we just blow it off, or its forbidden,
       * in which case we force a balacing split 
       * to be created.
       *
       * Note that its ok to have a single split who's amount is zero ..
       * this is just a split that is recording a price, and nothing
       * else.  (i.e. it still obeys the rule that the sum of the 
       * value of all the splits is zero).
       */

       if (force_double_entry) {
          if (! (DEQ (0.0, split->damount))) {
             value = split->share_price * split->damount;
   
             /* malloc a new split, mirror it to the source split */
             s = xaccMallocSplit ();
             s->damount = -value;
             free (s->memo);
             s->memo = strdup (split->memo);
             free (s->action);
             s->action = strdup (split->action);
   
             /* insert the new split into the transaction and 
              * the same account as the source split */
             MARK_SPLIT (s);
             xaccTransAppendSplit (trans, s); 
             xaccAccountInsertSplit (split->acc, s);
          }
       }
    }
  } else {

    /* The indicated split is a destination split.
     * Compute grand total of all destination splits,
     * and force the source split to blanace.
     */
    s = trans->splits[0];
    value = ComputeValue (trans->splits, s, base_currency);
    xaccSplitSetBaseValue (s, -value, base_currency);
    MARK_SPLIT (s);
    xaccAccountRecomputeBalance (s->acc); 
  }

  /* hack alert -- if the "force-double-entry" flag is set,
   * we should check to make sure that every split belongs
   * to some account.  If any of them don't, force them 
   * into the current account. If there's not current account,
   * force them into a lost & found account */
  /* hack alert -- implement the above */

}

/********************************************************************\
\********************************************************************/

#define CHECK_OPEN(trans) {					\
   if (!trans->open) {						\
      printf ("Error: transaction %p not open for editing\n", trans);	\
      /* assert (trans->open); */					\
      printf ("%s:%d \n", __FILE__, __LINE__);			\
      /* return; */						\
   }								\
}

void
xaccTransBeginEdit (Transaction *trans, int defer)
{
   assert (trans);
   trans->open = BEGIN_EDIT;
   if (defer) trans->open |= DEFER_REBALANCE;
   xaccOpenLog ();
   xaccTransWriteLog (trans, 'B');
}

void
xaccTransCommitEdit (Transaction *trans)
{
   int i;
   Split *split;
   Account *acc;

   if (!trans) return;
   CHECK_OPEN (trans);

   trans->open &= ~DEFER_REBALANCE;
   xaccTransRebalance (trans);

   /* um, theoritically, it is impossible for splits
    * to get inserted out of order. But we'll get paranoid,
    * and check anyway, at the loss of some performance.
    */
   i=0;
   split = trans->splits[i];
   while (split) {
      acc = split ->acc;
      xaccCheckDateOrder(acc, trans->splits[i]);
      i++;
      split = trans->splits[i];
   }

   i=0;
   split = trans->splits[i];
   while (split) {
      acc = split ->acc;
      xaccAccountRecomputeBalance (acc); 
      i++;
      split = trans->splits[i];
   }

   trans->open = 0;
   xaccTransWriteLog (trans, 'C');
}

/********************************************************************\
\********************************************************************/

void
xaccTransDestroy (Transaction *trans)
{
   int i;
   Split *split;
   Account *acc;

   if (!trans) return;
   CHECK_OPEN (trans);
   xaccTransWriteLog (trans, 'D');

   i=0;
   split = trans->splits[i];
   while (split) {
      MARK_SPLIT (split);
      acc = split ->acc;
      xaccAccountRemoveSplit (acc, split);
      xaccAccountRecomputeBalance (acc); 
      i++;
      split = trans->splits[i];
   }

   xaccFreeTransaction (trans);
}

/********************************************************************\
\********************************************************************/

void
xaccSplitDestroy (Split *split)
{
   Account *acc;
   Transaction *trans;
   int numsplits = 0;
   int ismember = 0;
   Split *s;

   trans = split->parent;
   assert (trans);
   assert (trans->splits);
   CHECK_OPEN (trans);

   numsplits = 0;
   s = trans->splits[0];
   while (s) {
/* xxxxxxx */
printf ("SplitDestroy(): trans=%p, %d'th split=%p\n", trans, numsplits, s);
      MARK_SPLIT(s);
      if (s == split) ismember = 1;
      numsplits ++;
      s = trans->splits[numsplits];
   }
   assert (ismember);

   /* if the account has three or more splits, 
    * merely unlink & free the split. 
    */
   if (2 < numsplits) {
      MARK_SPLIT (split);
      xaccTransRemoveSplit (trans, split);
      acc = split->acc;
      xaccAccountRemoveSplit (acc, split);
      xaccAccountRecomputeBalance (acc);
      xaccFreeSplit (split);
      xaccSplitRebalance (trans->splits[0]);
   } else {
      /* if the transaction has only two splits,
       * remove both of them, and them destroy the 
       * transaction. Make a log in the journal before 
       * destruction.
       */
      xaccTransWriteLog (trans, 'D');
      s = trans->splits[0];
      acc = s->acc;
      MARK_SPLIT (s);
      xaccAccountRemoveSplit (acc, s);
      xaccAccountRecomputeBalance (acc);

      s = trans->splits[1];
      if (s) {
         acc = s->acc;
         MARK_SPLIT (s);
         xaccAccountRemoveSplit (acc, s);
         xaccAccountRecomputeBalance (acc);
         xaccFreeTransaction (trans);
      }
   }
}

/********************************************************************\
\********************************************************************/

void
xaccTransAppendSplit (Transaction *trans, Split *split) 
{
   int i, num;
   Split **oldarray;
   Transaction *oldtrans;

   if (!trans) return;
   if (!split) return;

   CHECK_OPEN (trans);

   /* first, make sure that the split isn't already inserted 
    * elsewhere. If so, then remove it. */
   oldtrans = split->parent;
   if (oldtrans) {
      xaccTransRemoveSplit (oldtrans, split);
      xaccTransRebalance (oldtrans);
   }
   
   /* now, insert the split into the array */
   split->parent = trans;
   num = xaccCountSplits (trans->splits);

   oldarray = trans->splits;
   trans->splits = (Split **) _malloc ((num+2)*sizeof(Split *));
   for (i=0; i<num; i++) {
      (trans->splits)[i] = oldarray[i];
   }
   trans->splits[num] = split;
   trans->splits[num+1] = NULL;

   if (oldarray) _free (oldarray);

   /* force double entry to always be consistent */
   xaccSplitRebalance (split);
}

/********************************************************************\
 * TransRemoveSplit is an engine private function and does not/should
 * not cause any rebalancing to occur.
\********************************************************************/


void
xaccTransRemoveSplit (Transaction *trans, Split *split) 
{
   int i=0, n=0;
   Split *s;

   if (!split) return;
   if (!trans) return;
   split->parent = NULL;

   s = trans->splits[0];
   while (s) {
     trans->splits[i] = trans->splits[n];
     if (split == s) { i--; }
     i++;
     n++;
     s = trans->splits[n];
   }
   trans->splits[i] = NULL;
}

/********************************************************************\
 * sorting comparison function
 *
 * returns a negative value if transaction a is dated earlier than b, 
 * returns a positive value if transaction a is dated later than b, 
 *
 * This function tries very hard to uniquely order all transactions.
 * If two transactions occur on the same date, then thier "num" fields
 * are compared.  If the num fileds are identical, then the description
 * fileds are compared.  If these are identical, then the memo fileds 
 * are compared.  Hopefully, there will not be any transactions that
 * occur on the same day that have all three of these values identical.
 *
 * Note that being able to establish this kind of absolute order is 
 * important for some of the ledger display functions.  In particular,
 * grep for "running_balance" in the code, and see the notes there.
 *
 * Yes, this kindof code dependency is ugly, but the alternatives seem
 * ugly too.
 *
\********************************************************************/
int
xaccSplitOrder (Split **sa, Split **sb)
{
  int retval;
  char *da, *db;

  if ( (*sa) && !(*sb) ) return -1;
  if ( !(*sa) && (*sb) ) return +1;
  if ( !(*sa) && !(*sb) ) return 0;

  retval = xaccTransOrder ( ((Transaction **) &((*sa)->parent)), 
                            ((Transaction **) &((*sb)->parent)));
  if (0 != retval) return retval;

  /* otherwise, sort on memo strings */
  da = (*sa)->memo;
  db = (*sb)->memo;
  SAFE_STRCMP (da, db);

  /* otherwise, sort on action strings */
  da = (*sa)->action;
  db = (*sb)->action;
  SAFE_STRCMP (da, db);

  return 0;
}


int
xaccTransOrder (Transaction **ta, Transaction **tb)
{
  char *da, *db;

  if ( (*ta) && !(*tb) ) return -1;
  if ( !(*ta) && (*tb) ) return +1;
  if ( !(*ta) && !(*tb) ) return 0;

  /* if dates differ, return */
  if ( ((*ta)->date_posted.tv_sec) <
       ((*tb)->date_posted.tv_sec)) {
    return -1;
  } else
  if ( ((*ta)->date_posted.tv_sec) >
       ((*tb)->date_posted.tv_sec)) {
    return +1;
  }

  /* else, seconds match. check nanoseconds */
  if ( ((*ta)->date_posted.tv_nsec) <
       ((*tb)->date_posted.tv_nsec)) {
    return -1;
  } else
  if ( ((*ta)->date_posted.tv_nsec) >
       ((*tb)->date_posted.tv_nsec)) {
    return +1;
  }

  /* otherwise, sort on transaction strings */
  da = (*ta)->num;
  db = (*tb)->num;
  SAFE_STRCMP (da, db);

  /* otherwise, sort on transaction strings */
  da = (*ta)->description;
  db = (*tb)->description;
  SAFE_STRCMP (da, db);

  return 0;
}

/********************************************************************\
\********************************************************************/

int
xaccCountTransactions (Transaction **tarray)
{
   Transaction *trans;
   int ntrans = 0;

   if (!tarray) return 0;

   trans = tarray[0];
   while (trans) {
      ntrans ++;
      trans = tarray[ntrans];
   }
   return ntrans;
}

/********************************************************************\
\********************************************************************/

void
xaccTransSetDateSecs (Transaction *trans, time_t secs)
{
   Split *split;
   Account *acc;
   int i=0;

   if (!trans) return;
   CHECK_OPEN (trans);

   /* hack alert -- for right now, keep the posted and the entered
    * dates in sync.  Later, we'll have to split these up. */


   trans->date_entered.tv_sec = secs;
   trans->date_posted.tv_sec = secs;

   /* since the date has changed, we need to be careful to 
    * make sure all associated splits are in proper order
    * in thier accounts.  The easiest way of ensuring this
    * is to remove and reinsert every split. The reinsertion
    * process will place the split in the correct date-sorted
    * order.
    */

   assert (trans->splits);

   i=0;
   split = trans->splits[i];
   while (split) {
      acc = split->acc;
      xaccAccountRemoveSplit (acc, split);
      xaccAccountInsertSplit (acc, split);

      i++;
      split = trans->splits[i];
   }
}

void
xaccTransSetDate (Transaction *trans, int day, int mon, int year)
{
   struct tm date;
   time_t secs;

   date.tm_year = year - 1900;
   date.tm_mon = mon - 1;
   date.tm_mday = day;
   date.tm_hour = 11;
   date.tm_min = 0;
   date.tm_sec = 0;

   /* compute number of seconds */
   secs = mktime (&date);

   xaccTransSetDateSecs (trans, secs);
}

void
xaccTransSetDateToday (Transaction *trans)
{
   time_t secs;

   secs = time (0);
   xaccTransSetDateSecs (trans, secs);
}


/********************************************************************\
\********************************************************************/

void
xaccTransSetNum (Transaction *trans, const char *xnum)
{
   char * tmp;
   if (!trans) return;
   CHECK_OPEN (trans);

   tmp = strdup (xnum);
   if (trans->num) free (trans->num);
   trans->num = tmp;
   MarkChanged (trans);
}

void
xaccTransSetDescription (Transaction *trans, const char *desc)
{
   char * tmp;
   if (!trans) return;
   CHECK_OPEN (trans);

   tmp = strdup (desc);
   if (trans->description) free (trans->description);
   trans->description = tmp;
   MarkChanged (trans);
}

#define SET_TRANS_FIELD(trans,field,value)			\
{								\
   char * tmp;							\
   if (!trans) return;						\
   CHECK_OPEN (trans);						\
								\
   /* the engine *must* always be internally consistent */	\
   assert (trans->splits);					\
   assert (trans->splits[0]);					\
								\
   /* there must be two splits if value of one non-zero */	\
   if (force_double_entry) {					\
     if (! (DEQ (0.0, trans->splits[0]->damount))) {		\
        assert (trans->splits[1]);				\
     }								\
   }								\
								\
   tmp = strdup (value);					\
   free (trans->splits[0]->field);				\
   trans->splits[0]->field = tmp;				\
   MARK_SPLIT (trans->splits[0]);				\
								\
   /* If there are just two splits, then keep them in sync. */	\
   if (0x0 != trans->splits[1]) {				\
      if (0x0 == trans->splits[2]) {				\
         free (trans->splits[1]->field);			\
         trans->splits[1]->field = strdup (tmp);		\
         MARK_SPLIT (trans->splits[1]);				\
      }								\
   }								\
}

void
xaccTransSetMemo (Transaction *trans, const char *mimeo)
{
   SET_TRANS_FIELD (trans, memo, mimeo);
}

void
xaccTransSetAction (Transaction *trans, const char *actn)
{
   SET_TRANS_FIELD (trans, action, actn);
}

/********************************************************************\
\********************************************************************/

Split *
xaccTransGetSplit (Transaction *trans, int i) 
{
   if (!trans) return NULL;
   if (0 > i) return NULL;
   /* hack alert - should check if i > sizeof array */
   if (trans->splits) {
      return (trans->splits[i]);
   }
   return NULL;
}

char *
xaccTransGetNum (Transaction *trans)
{
   if (!trans) return NULL;
   return (trans->num);
}

char * 
xaccTransGetDescription (Transaction *trans)
{
   if (!trans) return NULL;
   return (trans->description);
}

time_t
xaccTransGetDate (Transaction *trans)
{
   if (!trans) return 0;
   return (trans->date_posted.tv_sec);
}

int 
xaccTransCountSplits (Transaction *trans)
{
   return (xaccCountSplits (trans->splits));
}

/********************************************************************\
\********************************************************************/

void
xaccSplitSetMemo (Split *split, const char *memo)
{
   char * tmp;
   if (!split) return;
   tmp = strdup (memo);
   if (split->memo) free (split->memo);
   split->memo = tmp;
   MARK_SPLIT (split);
}

void
xaccSplitSetAction (Split *split, const char *actn)
{
   char * tmp;
   if (!split) return;
   tmp = strdup (actn);
   if (split->action) free (split->action);
   split->action = tmp;
   MARK_SPLIT (split);
}

void
xaccSplitSetReconcile (Split *split, char recn)
{
   if (!split) return;
   split->reconciled = recn;
   MARK_SPLIT (split);
   xaccAccountRecomputeBalance (split->acc);
}

/********************************************************************\
\********************************************************************/

/* return the parent transaction of the split */
Transaction * 
xaccSplitGetParent (Split *split)
{
   if (!split) return NULL;
   return (split->parent);
}

Account *
xaccSplitGetAccount (Split *split)
{
   if (!split) return NULL;
   return (split->acc);
}

char *
xaccSplitGetMemo (Split *split)
{
   if (!split) return NULL;
   return (split->memo);
}

char *
xaccSplitGetAction (Split *split)
{
   if (!split) return NULL;
   return (split->action);
}

char 
xaccSplitGetReconcile (Split *split)
{
   if (!split) return ' ';
   return (split->reconciled);
}

double
xaccSplitGetShareAmount (Split * split)
{
   if (!split) return 0.0;
   return (split->damount);
}

double
xaccSplitGetValue (Split * split)
{
   if (!split) return 0.0;
   return ((split->damount) * (split->share_price));
}

double
xaccSplitGetSharePrice (Split * split)
{
   if (!split) return 1.0;
   return (split->share_price);
}

/********************************************************************\
\********************************************************************/

Account *
xaccGetAccountByName (Transaction *trans, const char * name)
{
   Split *s;
   Account *acc = NULL;
   int i;

   if (!trans) return NULL;
   if (!name) return NULL;

   /* walk through the splits, looking for one, any one, that has a parent account */
   i = 0;
   s = trans->splits[0];
   while (s) {
      acc = s->acc;
      if (acc) break;
      i++;
      s = trans->splits[i];
   }
   
   if (!acc) return 0x0;

   acc = xaccGetPeerAccountFromName (acc, name);
   return acc;
}

/********************************************************************\
\********************************************************************/

Split *
xaccGetOtherSplit (Split *split)
{
   Transaction *trans;

   if (!split) return NULL;
   trans = split->parent;

   /* if more than two splits, return NULL */
   if ((trans->splits[1]) && (trans->splits[2])) return NULL;

   if (split == trans->splits[0]) return (trans->splits[1]);
   if (split == trans->splits[1]) return (trans->splits[0]);
   return NULL;  /* never reached, in theory */
}

/********************************************************************\
\********************************************************************/

int
xaccIsPeerSplit (Split *sa, Split *sb)
{
   Transaction *ta, *tb;
   if (!sa || !sb) return 0;
   ta = sa->parent;
   tb = sb->parent;
   if (ta == tb) return 1;
   return 0;
}

/************************ END OF ************************************\
\************************* FILE *************************************/
