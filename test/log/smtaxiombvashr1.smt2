(set-option :incremental false)
(set-logic QF_BV)
(declare-fun s () (_ BitVec 1))
(declare-fun t () (_ BitVec 1))

(assert (not (= (bvashr s t) (ite (= ((_ extract 0 0) s) (_ bv0 1)) (bvlshr s t) (bvnot (bvlshr (bvnot s) t))))))



