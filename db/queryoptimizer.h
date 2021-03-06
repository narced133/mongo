// @file queryoptimizer.h

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "cursor.h"
#include "jsobj.h"
#include "queryutil.h"
#include "matcher.h"
#include "../util/message.h"

namespace mongo {

    class IndexDetails;
    class IndexType;

    /** A plan for executing a query using the given index spec and FieldRangeSet. */
    class QueryPlan : boost::noncopyable {
    public:

        QueryPlan(NamespaceDetails *d,
                  int idxNo, // -1 = no index
                  const FieldRangeSetPair &frsp,
                  const FieldRangeSetPair &originalFrsp,
                  const BSONObj &originalQuery,
                  const BSONObj &order,
                  const BSONObj &startKey = BSONObj(),
                  const BSONObj &endKey = BSONObj(),
                  string special="" );

        /** @return true iff no other plans should be considered. */
        bool optimal() const { return _optimal; }
        /* @return true iff this plan should not be considered at all. */
        bool unhelpful() const { return _unhelpful; }
        /** @return true iff ScanAndOrder processing will be required for result set. */
        bool scanAndOrderRequired() const { return _scanAndOrderRequired; }
        /**
         * @return true iff the index we are using has keys such that it can completely resolve the
         * query expression to match by itself without ever checking the main object.
         */
        bool exactKeyMatch() const { return _exactKeyMatch; }
        /** @return true iff this QueryPlan would perform an unindexed scan. */
        bool willScanTable() const { return _idxNo < 0 && !_impossible; }

        /** @return a new cursor based on this QueryPlan's index and FieldRangeSet. */
        shared_ptr<Cursor> newCursor( const DiskLoc &startLoc = DiskLoc() , int numWanted=0 ) const;
        /** @return a new reverse cursor if this is an unindexed plan. */
        shared_ptr<Cursor> newReverseCursor() const;
        /** Register this plan as a winner for its QueryPattern, with specified 'nscanned'. */
        void registerSelf( long long nScanned ) const;

        int direction() const { return _direction; }
        BSONObj indexKey() const;
        bool indexed() const { return _index; }
        int idxNo() const { return _idxNo; }
        const char *ns() const { return _frs.ns(); }
        NamespaceDetails *nsd() const { return _d; }
        BSONObj originalQuery() const { return _originalQuery; }
        BSONObj simplifiedQuery( const BSONObj& fields = BSONObj() ) const { return _frs.simplifiedQuery( fields ); }
        const FieldRange &range( const char *fieldName ) const { return _frs.range( fieldName ); }
        shared_ptr<FieldRangeVector> originalFrv() const { return _originalFrv; }

        /** just for testing */
        
        shared_ptr<FieldRangeVector> frv() const { return _frv; }
        bool isMultiKey() const;

    private:
        NamespaceDetails * _d;
        int _idxNo;
        const FieldRangeSet &_frs;
        const BSONObj &_originalQuery;
        const BSONObj &_order;
        const IndexDetails * _index;
        bool _optimal;
        bool _scanAndOrderRequired;
        bool _exactKeyMatch;
        int _direction;
        shared_ptr<FieldRangeVector> _frv;
        shared_ptr<FieldRangeVector> _originalFrv;
        BSONObj _startKey;
        BSONObj _endKey;
        bool _endKeyInclusive;
        bool _unhelpful;
        bool _impossible;
        string _special;
        IndexType * _type;
        bool _startOrEndSpec;
    };

    /**
     * Inherit from this interface to implement a new query operation.
     * The query optimizer will clone the QueryOp that is provided, giving
     * each clone its own query plan.
     *
     * Normal sequence of events:
     * 1) A new QueryOp is generated using createChild().
     * 2) A QueryPlan is assigned to this QueryOp with setQueryPlan().
     * 3) _init() is called on the QueryPlan.
     * 4) next() is called repeatedly, with nscanned() checked after each call.
     * 5) In one of these calls to next(), setComplete() is called.
     * 6) The QueryPattern for the QueryPlan may be recorded as a winner.
     */
    class QueryOp {
    public:
        QueryOp() : _complete(), _stopRequested(), _qp(), _error() {}

        /** Used when handing off from one QueryOp to another. */
        QueryOp( const QueryOp &other ) :
            _complete(), _stopRequested(), _qp(), _error(), _matcher( other._matcher ),
            _orConstraint( other._orConstraint ) {}

        virtual ~QueryOp() {}

        /** @return QueryPlan assigned to this QueryOp by the query optimizer. */
        const QueryPlan &qp() const { return *_qp; }
                
        /** Advance to next potential matching document (eg using a cursor). */
        virtual void next() = 0;
        /**
         * @return current 'nscanned' metric for this QueryOp.  Used to compare
         * cost to other QueryOps.
         */
        virtual long long nscanned() = 0;
        /** Take any steps necessary before the db mutex is yielded. */
        virtual bool prepareToYield() { massert( 13335, "yield not supported", false ); return false; }
        /** Recover once the db mutex is regained. */
        virtual void recoverFromYield() { massert( 13336, "yield not supported", false ); }
                
        /**
         * @return true iff the QueryPlan for this QueryOp may be registered
         * as a winning plan.
         */
        virtual bool mayRecordPlan() const = 0;

        /** @return true iff the implementation called setComplete() or setStop(). */
        bool complete() const { return _complete; }
        /** @return true iff the implementation called steStop(). */
        bool stopRequested() const { return _stopRequested; }
        /** @return true iff the implementation threw an exception. */
        bool error() const { return _error; }
        /** @return the exception thrown by implementation if one was thrown. */
        ExceptionInfo exception() const { return _exception; }
        
        /** To be called by QueryPlanSet::Runner only. */
        
        QueryOp *createChild();
        void setQueryPlan( const QueryPlan *qp ) { _qp = qp; assert( _qp != NULL ); }
        void init();        
        void setException( const DBException &e ) {
            _error = true;
            _exception = e.getInfo();
        }
        shared_ptr<CoveredIndexMatcher> matcher() const { return _matcher; }
        
    protected:
        /** Call if all results have been found. */
        void setComplete() {
            _orConstraint = qp().originalFrv();
            _complete = true;
        }
        /** Call if the scan is complete even if not all results have been found. */
        void setStop() { setComplete(); _stopRequested = true; }

        /** Handle initialization after a QueryPlan has been set. */
        virtual void _init() = 0;

        /** @return a copy of the inheriting class, which will be run with its own query plan. */
        virtual QueryOp *_createChild() const = 0;

        virtual bool alwaysUseRecord() const { return false; }

    private:
        bool _complete;
        bool _stopRequested;
        ExceptionInfo _exception;
        const QueryPlan *_qp;
        bool _error;
        shared_ptr<CoveredIndexMatcher> _matcher;
        shared_ptr<CoveredIndexMatcher> _oldMatcher;
        shared_ptr<FieldRangeVector> _orConstraint;
    };

    /**
     * A set of candidate query plans for a query.  This class can return a best buess plan or run a
     * QueryOp on all the plans.
     */
    class QueryPlanSet {
    public:

        typedef boost::shared_ptr<QueryPlan> QueryPlanPtr;
        typedef vector<QueryPlanPtr> PlanSet;

        QueryPlanSet( const char *ns,
                      auto_ptr<FieldRangeSetPair> frsp,
                      auto_ptr<FieldRangeSetPair> originalFrsp,
                      const BSONObj &originalQuery,
                      const BSONObj &order,
                      const BSONElement *hint = 0,
                      bool honorRecordedPlan = true,
                      const BSONObj &min = BSONObj(),
                      const BSONObj &max = BSONObj(),
                      bool bestGuessOnly = false,
                      bool mayYield = false);

        /** @return number of candidate plans. */
        int nPlans() const { return _plans.size(); }

        /**
         * Clone op for each query plan, and @return the first cloned op to call
         * setComplete() or setStop().
         */

        shared_ptr<QueryOp> runOp( QueryOp &op );
        template<class T>
        shared_ptr<T> runOp( T &op ) {
            return dynamic_pointer_cast<T>( runOp( static_cast<QueryOp&>( op ) ) );
        }

        /** @return metadata about cursors and index bounds for all plans, suitable for explain output. */
        BSONObj explain() const;
        /** @return true iff a plan is selected based on previous success of this plan. */
        bool usingPrerecordedPlan() const { return _usingPrerecordedPlan; }
        /** @return a single plan that may work well for the specified query. */
        QueryPlanPtr getBestGuess() const;

        //for testing
        const FieldRangeSetPair &frsp() const { return *_frsp; }
        const FieldRangeSetPair &originalFrsp() const { return *_originalFrsp; }
        bool modifiedKeys() const;
        bool hasMultiKey() const;

    private:
        void addOtherPlans( bool checkFirst );
        void addPlan( QueryPlanPtr plan, bool checkFirst ) {
            if ( checkFirst && plan->indexKey().woCompare( _plans[ 0 ]->indexKey() ) == 0 )
                return;
            _plans.push_back( plan );
        }
        void init();
        void addHint( IndexDetails &id );
        struct Runner {
            Runner( QueryPlanSet &plans, QueryOp &op );
            shared_ptr<QueryOp> run();
            void mayYield( const vector<shared_ptr<QueryOp> > &ops );
            QueryOp &_op;
            QueryPlanSet &_plans;
            static void initOp( QueryOp &op );
            static void nextOp( QueryOp &op );
            static bool prepareToYield( QueryOp &op );
            static void recoverFromYield( QueryOp &op );
        };

        const char *_ns;
        BSONObj _originalQuery;
        auto_ptr<FieldRangeSetPair> _frsp;
        auto_ptr<FieldRangeSetPair> _originalFrsp;
        PlanSet _plans;
        bool _mayRecordPlan;
        bool _usingPrerecordedPlan;
        BSONObj _hint;
        BSONObj _order;
        long long _oldNScanned;
        bool _honorRecordedPlan;
        BSONObj _min;
        BSONObj _max;
        string _special;
        bool _bestGuessOnly;
        bool _mayYield;
        ElapsedTracker _yieldSometimesTracker;
    };

    /** Handles $or type queries by generating a QueryPlanSet for each $or clause. */
    class MultiPlanScanner {
    public:
        MultiPlanScanner( const char *ns,
                          const BSONObj &query,
                          const BSONObj &order,
                          const BSONElement *hint = 0,
                          bool honorRecordedPlan = true,
                          const BSONObj &min = BSONObj(),
                          const BSONObj &max = BSONObj(),
                          bool bestGuessOnly = false,
                          bool mayYield = false);

        /**
         * Clone op for each query plan of a single $or clause, and @return the first cloned op
         * to call setComplete() or setStop().
         */

        shared_ptr<QueryOp> runOpOnce( QueryOp &op );
        template<class T>
        shared_ptr<T> runOpOnce( T &op ) {
            return dynamic_pointer_cast<T>( runOpOnce( static_cast<QueryOp&>( op ) ) );
        }

        /**
         * For each $or clause, calls runOpOnce on the child QueryOp cloned from the winning QueryOp
         * of the previous $or clause (or from the supplied 'op' for the first $or clause).
         */

        shared_ptr<QueryOp> runOp( QueryOp &op );
        template<class T>
        shared_ptr<T> runOp( T &op ) {
            return dynamic_pointer_cast<T>( runOp( static_cast<QueryOp&>( op ) ) );
        }

        /** @return true iff more $or clauses need to be scanned. */
        bool mayRunMore() const { return _or ? ( !_tableScanned && !_org.orFinished() ) : _i == 0; }
        /** @return non-$or version of explain output. */
        BSONObj oldExplain() const { assertNotOr(); return _currentQps->explain(); }
        /** @return true iff this is not a $or query and a plan is selected based on previous success of this plan. */
        bool usingPrerecordedPlan() const { return !_or && _currentQps->usingPrerecordedPlan(); }
        /** Don't attempt to scan multiple plans, just use the best guess. */
        void setBestGuessOnly() { _bestGuessOnly = true; }
        /** Yielding is allowed while running each QueryPlan. */
        void mayYield( bool val ) { _mayYield = val; }
        bool modifiedKeys() const { return _currentQps->modifiedKeys(); }
        bool hasMultiKey() const { return _currentQps->hasMultiKey(); }

    private:
        void assertNotOr() const {
            massert( 13266, "not implemented for $or query", !_or );
        }
        bool uselessOr( const BSONElement &hint ) const;
        const char * _ns;
        bool _or;
        BSONObj _query;
        OrRangeGenerator _org;
        auto_ptr<QueryPlanSet> _currentQps;
        int _i;
        bool _honorRecordedPlan;
        bool _bestGuessOnly;
        BSONObj _hint;
        bool _mayYield;
        bool _tableScanned;
    };

    /** Provides a cursor interface for certain limited uses of a MultiPlanScanner. */
    class MultiCursor : public Cursor {
    public:
        class CursorOp : public QueryOp {
        public:
            CursorOp() {}
            CursorOp( const QueryOp &other ) : QueryOp( other ) {}
            virtual shared_ptr<Cursor> newCursor() const = 0;
        };
        /** takes ownership of 'op' */
        MultiCursor( const char *ns, const BSONObj &pattern, const BSONObj &order, shared_ptr<CursorOp> op = shared_ptr<CursorOp>(), bool mayYield = false );
        /** used to handoff a query to a getMore() */
        MultiCursor( auto_ptr<MultiPlanScanner> mps, const shared_ptr<Cursor> &c, const shared_ptr<CoveredIndexMatcher> &matcher, const QueryOp &op );

        virtual bool ok() { return _c->ok(); }
        virtual Record* _current() { return _c->_current(); }
        virtual BSONObj current() { return _c->current(); }
        virtual DiskLoc currLoc() { return _c->currLoc(); }
        virtual bool advance() {
            _c->advance();
            while( !ok() && _mps->mayRunMore() ) {
                nextClause();
            }
            return ok();
        }
        virtual BSONObj currKey() const { return _c->currKey(); }
        virtual DiskLoc refLoc() { return _c->refLoc(); }
        virtual void noteLocation() { _c->noteLocation(); }
        virtual void checkLocation() { _c->checkLocation(); }
        virtual bool supportGetMore() { return true; }
        virtual bool supportYields() { return _c->supportYields(); }

        /**
         * with update we could potentially get the same document on multiple
         * indexes, but update appears to already handle this with seenObjects
         * so we don't have to do anything special here.
         */
        virtual bool getsetdup(DiskLoc loc) { return _c->getsetdup( loc ); }

        virtual bool modifiedKeys() const { return _mps->modifiedKeys(); }

        virtual bool isMultiKey() const { return _mps->hasMultiKey(); }

        virtual CoveredIndexMatcher *matcher() const { return _matcher.get(); }
        /** return -1 if we're a getmore handoff */
        virtual long long nscanned() { return _nscanned >= 0 ? _nscanned + _c->nscanned() : _nscanned; }
        /** just for testing */
        shared_ptr<Cursor> sub_c() const { return _c; }
    private:
        class NoOp : public CursorOp {
        public:
            NoOp() {}
            NoOp( const QueryOp &other ) : CursorOp( other ) {}
            virtual void _init() { setComplete(); }
            virtual void next() {}
            virtual bool mayRecordPlan() const { return false; }
            virtual QueryOp *_createChild() const { return new NoOp(); }
            virtual shared_ptr<Cursor> newCursor() const { return qp().newCursor(); }
            virtual long long nscanned() { assert( false ); return 0; }
        };
        void nextClause();
        shared_ptr<CursorOp> _op;
        shared_ptr<Cursor> _c;
        auto_ptr<MultiPlanScanner> _mps;
        shared_ptr<CoveredIndexMatcher> _matcher;
        long long _nscanned;
    };

    /** NOTE min, max, and keyPattern will be updated to be consistent with the selected index. */
    IndexDetails *indexDetailsForRange( const char *ns, string &errmsg, BSONObj &min, BSONObj &max, BSONObj &keyPattern );

    bool isSimpleIdQuery( const BSONObj& query );

    /**
     * @return a single cursor that may work well for the given query.  The returned cursor will
     * always have a matcher().
     * It is possible no cursor is returned if the sort is not supported by an index.  Clients are responsible
     * for checking this if they are not sure an index for a sort exists, and defaulting to a non-sort if
     * no suitable indices exist.
     */
    shared_ptr<Cursor> bestGuessCursor( const char *ns, const BSONObj &query, const BSONObj &sort );

    /**
     * Add-on functionality for queryutil classes requiring access to indexing
     * functionality not currently linked to mongos.
     * TODO Clean this up a bit, possibly with separate sharded and non sharded
     * implementations for the appropriate queryutil classes or by pulling index
     * related functionality into separate wrapper classes.
     */
    struct QueryUtilIndexed {
        /** @return true if the index may be useful according to its KeySpec. */
        static bool indexUseful( const FieldRangeSetPair &frsp, NamespaceDetails *d, int idxNo, const BSONObj &order );
        /** Clear any indexes recorded as the best for either the single or multi key pattern. */
        static void clearIndexesForPatterns( const FieldRangeSetPair &frsp, const BSONObj &order );
        /** Return a recorded best index for the single or multi key pattern. */
        static pair< BSONObj, long long > bestIndexForPatterns( const FieldRangeSetPair &frsp, const BSONObj &order );        
        static bool uselessOr( const OrRangeGenerator& org, NamespaceDetails *d, int hintIdx );
    };
    
} // namespace mongo
