(set-option :incremental false)
(set-logic QF_BV)
(declare-fun s () (_ BitVec 3))
(declare-fun t () (_ BitVec 3))

(assert (not (= (bvashr s t) (ite (= ((_ extract 2 2) s) (_ bv0 1)) (bvlshr s t) (bvnot (bvlshr (bvnot s) t))))))



